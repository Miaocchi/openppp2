import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { LaunchOptions } from '../models/ProfileModels';
import { OpenPpp2Native } from './OpenPpp2Native';

export enum VpnState {
  Disconnected = 0,
  Connecting = 1,
  Connected = 2,
  Disconnecting = 3
}

export interface VpnStatistics {
  txSpeedBytes: number;
  rxSpeedBytes: number;
  inBytes: number;
  outBytes: number;
}

export interface SessionStartResult {
  ok: boolean;
  error?: string;
}

export class VpnNativeSession {
  private running: boolean = false;
  private runPromise?: Promise<number>;
  private lastError: string = '';

  get isRunning(): boolean {
    return this.running;
  }

  get errorText(): string {
    return this.lastError;
  }

  async start(context: common.Context, configJson: string, options: LaunchOptions, tunFd: number): Promise<SessionStartResult> {
    if (this.running) {
      return { ok: false, error: 'VPN 已在运行，请先断开再重连' };
    }

    const rootPath = this.ensureRuntimeDirectories(context);
    OpenPpp2Native.setRootPath(rootPath);

    const configResult = OpenPpp2Native.setAppConfiguration(configJson);
    if (configResult !== 0) {
      this.lastError = `setAppConfiguration failed: ${configResult}, ${OpenPpp2Native.getLastErrorText()}`;
      return { ok: false, error: this.lastError };
    }

    if (!options.proxyOnly && options.bypassIpList.trim().length > 0) {
      OpenPpp2Native.setBypassIpList(options.bypassIpList);
    }
    if (!options.proxyOnly && options.dnsRulesList.trim().length > 0) {
      OpenPpp2Native.setDnsRulesList(options.dnsRulesList);
    }

    const niResult = OpenPpp2Native.setNetworkInterface({
      tunFd,
      mux: options.mux,
      vnet: options.vnet,
      blockQuic: options.blockQuic,
      staticMode: options.staticMode,
      ip: options.tunIp,
      mask: options.tunMask,
      gateway: options.gateway
    });
    if (niResult !== 0) {
      this.lastError = `setNetworkInterface failed: ${niResult}, ${OpenPpp2Native.getLastErrorText()}`;
      return { ok: false, error: this.lastError };
    }

    this.running = true;
    this.lastError = '';
    this.runPromise = OpenPpp2Native.run(0)
      .then((result: number) => {
        this.running = false;
        if (result !== 0) {
          this.lastError = `run returned ${result}, ${OpenPpp2Native.getLastErrorText()}`;
        }
        return result;
      })
      .catch((err: Error) => {
        this.running = false;
        this.lastError = err.message;
        return -1;
      });
    return { ok: true };
  }

  async stop(): Promise<number> {
    if (!this.running) {
      return 0;
    }
    const result = OpenPpp2Native.stop();
    await this.runPromise;
    this.running = false;
    return result;
  }

  getLinkState(): number {
    return OpenPpp2Native.getLinkState();
  }

  getStatistics(): VpnStatistics {
    try {
      const raw = OpenPpp2Native.getStatistics();
      const parsed = JSON.parse(raw) as Record<string, string>;
      return {
        txSpeedBytes: Number(parsed['tx'] ?? 0),
        rxSpeedBytes: Number(parsed['rx'] ?? 0),
        inBytes: Number(parsed['in'] ?? 0),
        outBytes: Number(parsed['out'] ?? 0)
      };
    } catch (_) {
      return { txSpeedBytes: 0, rxSpeedBytes: 0, inBytes: 0, outBytes: 0 };
    }
  }

  smokeConfigure(context: common.Context, configJson: string): SessionStartResult {
    const rootPath = this.ensureRuntimeDirectories(context);
    OpenPpp2Native.setRootPath(rootPath);
    const result = OpenPpp2Native.setAppConfiguration(configJson);
    if (result === 0) {
      return { ok: true };
    }
    return { ok: false, error: `native config smoke failed: ${result}, ${OpenPpp2Native.getLastErrorText()}` };
  }

  private ensureRuntimeDirectories(context: common.Context): string {
    const root = context.filesDir;
    this.mkdirIfMissing(`${root}/rules`);
    this.mkdirIfMissing(`${root}/generated`);
    return root;
  }

  private mkdirIfMissing(path: string): void {
    try {
      if (!fs.accessSync(path)) {
        fs.mkdirSync(path);
      }
    } catch (_) {
      try {
        fs.mkdirSync(path);
      } catch (_) {
      }
    }
  }
}

export const vpnNativeSession = new VpnNativeSession();
