import { Want } from '@kit.AbilityKit';
import { VpnExtensionAbility, vpnExtension } from '@kit.NetworkKit';
import { LaunchOptions, cloneOptions } from '../models/ProfileModels';
import { vpnNativeSession } from '../services/VpnNativeSession';

export default class OpenPpp2VpnExtensionAbility extends VpnExtensionAbility {
  private vpnConnection?: vpnExtension.VpnConnection;

  onCreate(want: Want): void {
    this.startFromWant(want);
  }

  onDestroy(): void {
    this.stopSession();
  }

  private async startFromWant(want: Want): Promise<void> {
    const params = want.parameters ?? {};
    const configJson = String(params['configJson'] ?? '');
    const optionsJson = String(params['optionsJson'] ?? '{}');
    if (configJson.trim().length === 0) {
      return;
    }

    const options = cloneOptions(JSON.parse(optionsJson) as Partial<LaunchOptions>);
    const tunFd = await this.createVpnInterface(options);
    if (tunFd < 0) {
      vpnNativeSession.smokeConfigure(this.context, configJson);
      return;
    }
    await vpnNativeSession.start(this.context, configJson, options, tunFd);
  }

  private async createVpnInterface(options: LaunchOptions): Promise<number> {
    try {
      this.vpnConnection = vpnExtension.createVpnConnection(this.context);
      const addressFamilyIpv4 = 1;
      const config: vpnExtension.VpnConfig = {
        addresses: [
          {
            address: {
              address: options.tunIp,
              family: addressFamilyIpv4
            },
            prefixLength: options.tunPrefix
          }
        ],
        routes: [
          {
            destination: {
              address: {
                address: options.proxyOnly ? options.tunIp : options.route,
                family: addressFamilyIpv4
              },
              prefixLength: options.proxyOnly ? options.tunPrefix : options.routePrefix
            },
            gateway: {
              address: options.gateway,
              family: addressFamilyIpv4
            },
            hasGateway: false,
            isDefaultRoute: !options.proxyOnly
          }
        ],
        dnsAddresses: [options.dns1, options.dns2].filter((dns: string) => dns.trim().length > 0),
        mtu: options.mtu,
        isIPv4Accepted: true,
        isIPv6Accepted: false,
        isBlocking: true,
        trustedApplications: [],
        blockedApplications: []
      };
      return Number(await this.vpnConnection.create(config));
    } catch (_) {
      return -1;
    }
    return -1;
  }

  private async destroyVpnConnection(): Promise<void> {
    if (this.vpnConnection === undefined) {
      return;
    }
    try {
      await this.vpnConnection.destroy();
    } catch (_) {
    } finally {
      this.vpnConnection = undefined;
    }
  }

  private async stopSession(): Promise<void> {
    await vpnNativeSession.stop();
    await this.destroyVpnConnection();
  }
}
