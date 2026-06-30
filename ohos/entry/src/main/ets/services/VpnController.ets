import { common, Want } from '@kit.AbilityKit';
import { vpnExtension } from '@kit.NetworkKit';
import { ConfigProfile, LaunchOptions } from '../models/ProfileModels';
import { profileStore } from './ProfileStore';
import { VpnState, VpnStatistics, vpnNativeSession } from './VpnNativeSession';

export type VpnStateListener = (state: VpnState) => void;
export type VpnStatsListener = (stats: VpnStatistics) => void;
export type VpnErrorListener = (message: string) => void;

const BUNDLE_NAME = 'io.github.openppp2.ohos';
const VPN_EXTENSION_NAME = 'OpenPpp2VpnExtensionAbility';

export class VpnController {
  private stateValue: VpnState = VpnState.Disconnected;
  private statsValue: VpnStatistics = { txSpeedBytes: 0, rxSpeedBytes: 0, inBytes: 0, outBytes: 0 };
  private errorValue: string = '';
  private stateListeners: VpnStateListener[] = [];
  private statsListeners: VpnStatsListener[] = [];
  private errorListeners: VpnErrorListener[] = [];
  private pollTimer: number = -1;

  get state(): VpnState {
    return this.stateValue;
  }

  get stats(): VpnStatistics {
    return this.statsValue;
  }

  get lastError(): string {
    return this.errorValue;
  }

  onState(listener: VpnStateListener): void {
    this.stateListeners.push(listener);
  }

  onStats(listener: VpnStatsListener): void {
    this.statsListeners.push(listener);
  }

  onError(listener: VpnErrorListener): void {
    this.errorListeners.push(listener);
  }

  async connect(context: common.UIAbilityContext, profile: ConfigProfile): Promise<boolean> {
    if (this.stateValue === VpnState.Connecting || this.stateValue === VpnState.Connected) {
      return false;
    }

    const telemetry = await profileStore.getTelemetrySettings();
    const effectiveJson = profileStore.buildEffectiveJson(profile, telemetry);
    this.setState(VpnState.Connecting);
    this.errorValue = '';

    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: VPN_EXTENSION_NAME,
      parameters: {
        configJson: effectiveJson,
        optionsJson: JSON.stringify(profile.options)
      }
    };

    try {
      await vpnExtension.startVpnExtensionAbility(want);
      this.startPolling();
      return true;
    } catch (err) {
      const smoke = vpnNativeSession.smokeConfigure(context, effectiveJson);
      this.errorValue = smoke.ok
        ? '系统未启动 VPN Extension；当前只完成 native 配置解析 smoke。请检查 VPN API 权限/签名。'
        : String(smoke.error ?? err);
      this.emitError(this.errorValue);
      this.setState(VpnState.Disconnected);
      return false;
    }
  }

  async disconnect(): Promise<void> {
    if (this.stateValue === VpnState.Disconnected) {
      return;
    }
    this.setState(VpnState.Disconnecting);
    await vpnNativeSession.stop();
    await vpnExtension.stopVpnExtensionAbility({
      bundleName: BUNDLE_NAME,
      abilityName: VPN_EXTENSION_NAME
    });
    this.stopPolling();
    this.statsValue = { txSpeedBytes: 0, rxSpeedBytes: 0, inBytes: 0, outBytes: 0 };
    this.setState(VpnState.Disconnected);
  }

  refreshSnapshot(): void {
    const linkState = vpnNativeSession.getLinkState();
    this.statsValue = vpnNativeSession.getStatistics();
    this.statsListeners.forEach((listener) => listener(this.statsValue));
    if (linkState === 0) {
      this.setState(VpnState.Connected);
    } else if (vpnNativeSession.isRunning) {
      this.setState(VpnState.Connecting);
    }
  }

  private startPolling(): void {
    this.stopPolling();
    this.pollTimer = setInterval(() => this.refreshSnapshot(), 1000);
  }

  private stopPolling(): void {
    if (this.pollTimer >= 0) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
  }

  private setState(state: VpnState): void {
    if (this.stateValue === state) {
      return;
    }
    this.stateValue = state;
    this.stateListeners.forEach((listener) => listener(state));
  }

  private emitError(message: string): void {
    this.errorListeners.forEach((listener) => listener(message));
  }
}

export const vpnController = new VpnController();
