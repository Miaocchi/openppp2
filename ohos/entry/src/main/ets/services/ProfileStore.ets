import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import {
  ConfigProfile,
  DEFAULT_TELEMETRY,
  LaunchOptions,
  RemoteSubscriptionResult,
  TelemetrySettings,
  cloneOptions,
  createDefaultProfile
} from '../models/ProfileModels';

const STORE_NAME = 'openppp2_profile_store';
const PROFILES_KEY = 'profiles_v1';
const ACTIVE_ID_KEY = 'active_profile_id';
const TELEMETRY_KEY = 'telemetry_v1';

export class ProfileStore {
  private prefs?: preferences.Preferences;

  async init(context: common.Context): Promise<void> {
    if (this.prefs !== undefined) {
      return;
    }
    this.prefs = await preferences.getPreferences(context, STORE_NAME);
    const profiles = await this.getProfiles();
    if (profiles.length === 0) {
      const profile = createDefaultProfile();
      await this.saveProfiles([profile], profile.id);
    }
  }

  async getProfiles(): Promise<ConfigProfile[]> {
    const raw = String(this.prefs?.getSync(PROFILES_KEY, '[]') ?? '[]');
    try {
      const parsed = JSON.parse(raw) as ConfigProfile[];
      return parsed.map((profile: ConfigProfile): ConfigProfile => {
        profile.options = cloneOptions(profile.options);
        profile.history = profile.history ?? [];
        return profile;
      });
    } catch (_) {
      return [];
    }
  }

  async getActiveProfile(): Promise<ConfigProfile | undefined> {
    const profiles = await this.getProfiles();
    const activeId = String(this.prefs?.getSync(ACTIVE_ID_KEY, '') ?? '');
    return profiles.find((profile: ConfigProfile) => profile.id === activeId) ?? profiles[0];
  }

  async setActiveProfile(id: string): Promise<void> {
    this.prefs?.putSync(ACTIVE_ID_KEY, id);
    await this.prefs?.flush();
  }

  async upsert(profile: ConfigProfile): Promise<void> {
    const profiles = await this.getProfiles();
    const index = profiles.findIndex((item: ConfigProfile) => item.id === profile.id);
    if (index >= 0) {
      const previous = profiles[index];
      const history = previous.json === profile.json
        ? previous.history
        : [{ timestampMs: Date.now(), json: previous.json }, ...(previous.history ?? [])].slice(0, 8);
      profiles[index] = Object.assign({}, profile, { history });
    } else {
      profiles.push(profile);
    }
    await this.saveProfiles(profiles, profile.id);
  }

  async deleteProfile(id: string): Promise<void> {
    const profiles = (await this.getProfiles()).filter((profile: ConfigProfile) => profile.id !== id);
    if (profiles.length === 0) {
      const profile = createDefaultProfile();
      await this.saveProfiles([profile], profile.id);
      return;
    }
    const active = await this.getActiveProfile();
    await this.saveProfiles(profiles, active?.id === id ? profiles[0].id : active?.id);
  }

  async importSubscription(url: string, subscription: RemoteSubscriptionResult): Promise<number> {
    const profiles = await this.getProfiles();
    const now = Date.now();
    subscription.nodes.forEach((node) => {
      const existingIndex = profiles.findIndex((profile: ConfigProfile) =>
        profile.subscriptionUrl === url && profile.subscriptionNodeId === node.id);
      const profile: ConfigProfile = {
        id: existingIndex >= 0 ? profiles[existingIndex].id : `sub-${node.id}-${now}`,
        name: node.name,
        subtitle: node.subtitle,
        flag: node.flag,
        json: node.json,
        favorite: existingIndex >= 0 ? profiles[existingIndex].favorite : false,
        subscriptionUrl: url,
        subscriptionNodeId: node.id,
        subscriptionUpdatedAtMs: now,
        options: cloneOptions(node.options),
        history: existingIndex >= 0 ? profiles[existingIndex].history : []
      };
      if (existingIndex >= 0) {
        profiles[existingIndex] = profile;
      } else {
        profiles.push(profile);
      }
    });
    await this.saveProfiles(profiles, profiles[0]?.id);
    return subscription.nodes.length;
  }

  async getTelemetrySettings(): Promise<TelemetrySettings> {
    const raw = String(this.prefs?.getSync(TELEMETRY_KEY, '') ?? '');
    if (raw.length === 0) {
      return Object.assign({}, DEFAULT_TELEMETRY);
    }
    try {
      return Object.assign({}, DEFAULT_TELEMETRY, JSON.parse(raw)) as TelemetrySettings;
    } catch (_) {
      return Object.assign({}, DEFAULT_TELEMETRY);
    }
  }

  async setTelemetrySettings(value: TelemetrySettings): Promise<void> {
    this.prefs?.putSync(TELEMETRY_KEY, JSON.stringify(Object.assign({}, DEFAULT_TELEMETRY, value)));
    await this.prefs?.flush();
  }

  async updateOptions(profileId: string, options: LaunchOptions): Promise<void> {
    const profiles = await this.getProfiles();
    const index = profiles.findIndex((profile: ConfigProfile) => profile.id === profileId);
    if (index < 0) {
      return;
    }
    profiles[index].options = cloneOptions(options);
    await this.saveProfiles(profiles, profileId);
  }

  buildEffectiveJson(profile: ConfigProfile, telemetry?: TelemetrySettings): string {
    let root = JSON.parse(profile.json) as Record<string, any>;
    const options = cloneOptions(profile.options);
    const client = Object.assign({}, root['client']) as Record<string, any>;
    const httpProxy = Object.assign({}, client['http-proxy']) as Record<string, any>;
    const socksProxy = Object.assign({}, client['socks-proxy']) as Record<string, any>;
    if (options.allowLan) {
      httpProxy['bind'] = '0.0.0.0';
      socksProxy['bind'] = '0.0.0.0';
    }
    client['http-proxy'] = httpProxy;
    client['socks-proxy'] = socksProxy;
    client['proxy-only'] = options.proxyOnly;
    root['client'] = client;

    const dnsConfig = options.dnsConfig;
    root['dns'] = {
      servers: {
        domestic: String(dnsConfig['domestic'] ?? 'doh.pub'),
        foreign: String(dnsConfig['foreign'] ?? 'cloudflare')
      },
      'intercept-unmatched': dnsConfig['interceptUnmatched'] === true,
      ecs: {
        enabled: dnsConfig['ecsEnabled'] === true,
        'override-ip': String(dnsConfig['ecsOverrideIp'] ?? '')
      },
      tls: {
        'verify-peer': dnsConfig['tlsVerifyPeer'] !== false
      },
      stun: {
        candidates: String(dnsConfig['stunCandidates'] ?? '')
          .split('\n')
          .map((line: string) => line.trim())
          .filter((line: string) => line.length > 0)
      }
    };

    const geo = options.geoRules;
    root['geo-rules'] = {
      enabled: geo['enabled'] === true,
      country: String(geo['country'] ?? 'cn'),
      'geoip-dat': String(geo['geoipDat'] ?? './rules/GeoIP.dat'),
      'geosite-dat': String(geo['geositeDat'] ?? './rules/GeoSite.dat'),
      'geoip-download-url': String(geo['geoipDownloadUrl'] ?? ''),
      'geosite-download-url': String(geo['geositeDownloadUrl'] ?? ''),
      geoip: String(geo['geoipFiles'] ?? '').split('\n').filter((line: string) => line.length > 0),
      geosite: String(geo['geositeFiles'] ?? '').split('\n').filter((line: string) => line.length > 0),
      'dns-provider-domestic': String(geo['dnsProviderDomestic'] ?? 'doh.pub'),
      'dns-provider-foreign': String(geo['dnsProviderForeign'] ?? 'cloudflare'),
      'output-bypass': String(geo['outputBypass'] ?? './generated/bypass-cn.txt'),
      'output-dns-rules': String(geo['outputDnsRules'] ?? './generated/dns-rules-cn.txt')
    };

    if (telemetry !== undefined) {
      root['telemetry'] = {
        enabled: telemetry.enabled,
        endpoint: telemetry.endpoint,
        level: telemetry.level,
        count: telemetry.metricsEnabled,
        span: telemetry.spansEnabled,
        'log-file': telemetry.logFile,
        'console-log': telemetry.consoleLog,
        'console-metric': telemetry.consoleMetric,
        'console-span': telemetry.consoleSpan
      };
    }
    return JSON.stringify(root, null, 2);
  }

  private async saveProfiles(profiles: ConfigProfile[], activeId?: string): Promise<void> {
    this.prefs?.putSync(PROFILES_KEY, JSON.stringify(profiles));
    if (activeId !== undefined && activeId.length > 0) {
      this.prefs?.putSync(ACTIVE_ID_KEY, activeId);
    }
    await this.prefs?.flush();
  }
}

export const profileStore = new ProfileStore();
