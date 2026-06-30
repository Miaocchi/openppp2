export interface NetworkInterfaceOptions {
  tunFd: number;
  mux: number;
  vnet: boolean;
  blockQuic: boolean;
  staticMode: boolean;
  ip: string;
  mask: string;
  gateway: string;
}

export function getDefaultCipherSuites(): string;
export function setRootPath(path: string): boolean;
export function setAppConfiguration(configurations: string): number;
export function getAppConfiguration(): string;
export function setNetworkInterface(options: NetworkInterfaceOptions): number;
export function getNetworkInterface(): string;
export function setBypassIpList(ipList: string): boolean;
export function setDnsRulesList(rules: string): boolean;
export function run(key: number): Promise<number>;
export function stop(): number;
export function clearConfigure(): void;
export function getLinkState(): number;
export function getStatistics(): string;
export function getLastErrorCode(): number;
export function getLastErrorText(): string;
