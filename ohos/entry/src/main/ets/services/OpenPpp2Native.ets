import openppp2 from 'libopenppp2.so';

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

interface NativeModule {
  getDefaultCipherSuites(): string;
  setRootPath(path: string): boolean;
  setAppConfiguration(configurations: string): number;
  getAppConfiguration(): string;
  setNetworkInterface(options: NetworkInterfaceOptions): number;
  getNetworkInterface(): string;
  setBypassIpList(ipList: string): boolean;
  setDnsRulesList(rules: string): boolean;
  run(key: number): Promise<number>;
  stop(): number;
  clearConfigure(): void;
  getLinkState(): number;
  getStatistics(): string;
  getLastErrorCode(): number;
  getLastErrorText(): string;
}

const nativeModule = openppp2 as NativeModule;

export class OpenPpp2Native {
  static getDefaultCipherSuites(): string {
    return nativeModule.getDefaultCipherSuites();
  }

  static setRootPath(path: string): boolean {
    return nativeModule.setRootPath(path);
  }

  static setAppConfiguration(configurations: string): number {
    return nativeModule.setAppConfiguration(configurations);
  }

  static getAppConfiguration(): string {
    return nativeModule.getAppConfiguration();
  }

  static setNetworkInterface(options: NetworkInterfaceOptions): number {
    return nativeModule.setNetworkInterface(options);
  }

  static getNetworkInterface(): string {
    return nativeModule.getNetworkInterface();
  }

  static setBypassIpList(ipList: string): boolean {
    return nativeModule.setBypassIpList(ipList);
  }

  static setDnsRulesList(rules: string): boolean {
    return nativeModule.setDnsRulesList(rules);
  }

  static run(key: number): Promise<number> {
    return nativeModule.run(key);
  }

  static stop(): number {
    return nativeModule.stop();
  }

  static clearConfigure(): void {
    nativeModule.clearConfigure();
  }

  static getLinkState(): number {
    return nativeModule.getLinkState();
  }

  static getStatistics(): string {
    return nativeModule.getStatistics();
  }

  static getLastErrorCode(): number {
    return nativeModule.getLastErrorCode();
  }

  static getLastErrorText(): string {
    return nativeModule.getLastErrorText();
  }
}
