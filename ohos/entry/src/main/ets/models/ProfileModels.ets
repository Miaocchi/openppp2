export interface ConfigSnapshot {
  timestampMs: number;
  json: string;
}

export interface LaunchOptions {
  tunIp: string;
  tunMask: string;
  tunPrefix: number;
  gateway: string;
  route: string;
  routePrefix: number;
  dns1: string;
  dns2: string;
  mtu: number;
  mux: number;
  vnet: boolean;
  blockQuic: boolean;
  staticMode: boolean;
  bypassIpList: string;
  dnsRulesList: string;
  allowLan: boolean;
  proxyOnly: boolean;
  dnsConfig: Record<string, any>;
  geoRules: Record<string, any>;
}

export interface TelemetrySettings {
  enabled: boolean;
  endpoint: string;
  level: number;
  metricsEnabled: boolean;
  spansEnabled: boolean;
  logFile: string;
  consoleLog: boolean;
  consoleMetric: boolean;
  consoleSpan: boolean;
}

export interface ConfigProfile {
  id: string;
  name: string;
  subtitle: string;
  flag: string;
  json: string;
  favorite: boolean;
  subscriptionUrl?: string;
  subscriptionNodeId?: string;
  subscriptionUpdatedAtMs?: number;
  options: LaunchOptions;
  history: ConfigSnapshot[];
}

export interface RemoteSubscriptionNode {
  id: string;
  name: string;
  subtitle: string;
  flag: string;
  json: string;
  options: Partial<LaunchOptions>;
}

export interface RemoteSubscriptionResult {
  name: string;
  profilePrefix?: string;
  nodes: RemoteSubscriptionNode[];
}

export const DEFAULT_CONFIG_JSON: string = `{
  "concurrent": 1,
  "cdn": [80, 443],
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "protocol": "aes-128-cfb",
    "protocol-key": "N6HMzdUs7IUnYHwq",
    "transport": "aes-256-cfb",
    "transport-key": "HWFweXu2g5RVMEpy",
    "masked": false,
    "plaintext": false,
    "delta-encode": false,
    "shuffle-data": false
  },
  "ip": { "public": "0.0.0.0", "interface": "0.0.0.0" },
  "vmem": { "size": 0, "path": "./" },
  "server": {
    "node": 1,
    "log": "./ppp.log",
    "subnet": true,
    "mapping": false,
    "backend": "",
    "backend-key": ""
  },
  "tcp": {
    "inactive": { "timeout": 300 },
    "connect": { "timeout": 5 },
    "listen": { "port": 20000 },
    "turbo": true,
    "backlog": 511,
    "fast-open": true
  },
  "udp": {
    "inactive": { "timeout": 72 },
    "dns": { "timeout": 4, "redirect": "8.8.8.8:53" },
    "listen": { "port": 20000 },
    "static": {
      "keep-alived": [1, 5],
      "dns": true,
      "quic": false,
      "icmp": true,
      "server": "127.0.0.1:20000"
    }
  },
  "websocket": {
    "host": "",
    "path": "/",
    "listen": { "ws": 0, "wss": 0 },
    "ssl": {
      "certificate-file": "",
      "certificate-chain-file": "",
      "certificate-key-file": "",
      "certificate-key-password": "",
      "ciphersuites": "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256"
    },
    "verify-peer": true,
    "http": {
      "error": { "root": "wwwroot/" },
      "request": {
        "Cache-Control": "no-cache",
        "Pragma": "no-cache",
        "X-Powered-By": "ASP.NET",
        "Content-Type": "text/html; charset=utf-8",
        "Server": "Microsoft-IIS/10.0"
      }
    }
  },
  "client": {
    "guid": "{F4569420-4E49-4CBA-9C36-94E722C8E363}",
    "server": "ppp://192.168.0.24:20000/",
    "bandwidth": 0,
    "reconnections": { "timeout": 5 },
    "paper-airplane": { "tcp": true },
    "http-proxy": { "bind": "127.0.0.1", "port": 8080 },
    "socks-proxy": { "bind": "127.0.0.1", "port": 1080 },
    "mappings": []
  }
}`;

export const DEFAULT_OPTIONS: LaunchOptions = {
  tunIp: '10.0.0.2',
  tunMask: '255.255.255.0',
  tunPrefix: 24,
  gateway: '10.0.0.1',
  route: '0.0.0.0',
  routePrefix: 0,
  dns1: '8.8.8.8',
  dns2: '1.1.1.1',
  mtu: 1400,
  mux: 0,
  vnet: false,
  blockQuic: true,
  staticMode: false,
  bypassIpList: '',
  dnsRulesList: '',
  allowLan: false,
  proxyOnly: false,
  dnsConfig: {
    domestic: 'doh.pub',
    foreign: 'cloudflare',
    interceptUnmatched: true,
    ecsEnabled: true,
    ecsOverrideIp: '',
    tlsVerifyPeer: true,
    stunCandidates: '39.107.142.158:3478\n74.125.250.129:19302'
  },
  geoRules: {
    enabled: true,
    country: 'cn',
    geoipDat: './rules/GeoIP.dat',
    geositeDat: './rules/GeoSite.dat',
    geoipDownloadUrl: 'https://testingcf.jsdelivr.net/gh/MetaCubeX/meta-rules-dat@release/geoip.dat',
    geositeDownloadUrl: 'https://testingcf.jsdelivr.net/gh/MetaCubeX/meta-rules-dat@release/geosite.dat',
    geoipFiles: './rules/geoip-cn.txt',
    geositeFiles: './rules/geosite-cn.txt',
    dnsProviderDomestic: 'doh.pub',
    dnsProviderForeign: 'cloudflare',
    outputBypass: './generated/bypass-cn.txt',
    outputDnsRules: './generated/dns-rules-cn.txt'
  }
};

export const DEFAULT_TELEMETRY: TelemetrySettings = {
  enabled: false,
  endpoint: '',
  level: 1,
  metricsEnabled: false,
  spansEnabled: false,
  logFile: '',
  consoleLog: true,
  consoleMetric: false,
  consoleSpan: false
};

export function cloneOptions(value?: Partial<LaunchOptions>): LaunchOptions {
  const merged = Object.assign({}, DEFAULT_OPTIONS, value ?? {}) as LaunchOptions;
  merged.dnsConfig = Object.assign({}, DEFAULT_OPTIONS.dnsConfig, value?.dnsConfig ?? {});
  merged.geoRules = Object.assign({}, DEFAULT_OPTIONS.geoRules, value?.geoRules ?? {});
  return merged;
}

export function createDefaultProfile(): ConfigProfile {
  return {
    id: `local-${Date.now()}`,
    name: '默认服务器',
    subtitle: 'ppp://192.168.0.24:20000/',
    flag: '',
    json: DEFAULT_CONFIG_JSON,
    favorite: true,
    options: cloneOptions(),
    history: []
  };
}
