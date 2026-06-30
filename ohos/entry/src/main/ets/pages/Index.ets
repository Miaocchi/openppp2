import { promptAction } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { ConfigProfile, LaunchOptions, TelemetrySettings, cloneOptions, createDefaultProfile } from '../models/ProfileModels';
import { profileStore } from '../services/ProfileStore';
import { SubscriptionService } from '../services/SubscriptionService';
import { vpnController } from '../services/VpnController';
import { VpnState, VpnStatistics } from '../services/VpnNativeSession';
import { OpenPpp2Native } from '../services/OpenPpp2Native';

function stateLabel(state: VpnState): string {
  switch (state) {
    case VpnState.Connecting:
      return '连接中';
    case VpnState.Connected:
      return '已连接';
    case VpnState.Disconnecting:
      return '断开中';
    default:
      return '未连接';
  }
}

function formatBytes(value: number): string {
  if (value > 1024 * 1024) {
    return `${(value / 1024 / 1024).toFixed(1)} MB`;
  }
  if (value > 1024) {
    return `${(value / 1024).toFixed(1)} KB`;
  }
  return `${value} B`;
}

@Entry
@Component
struct Index {
  @State private selectedIndex: number = 0;

  build() {
    Tabs({ barPosition: BarPosition.End }) {
      TabContent() {
        HomeTab()
      }
      .tabBar('主页')

      TabContent() {
        ProfilesTab()
      }
      .tabBar('服务器')

      TabContent() {
        OptionsTab()
      }
      .tabBar('参数')

      TabContent() {
        SettingsTab()
      }
      .tabBar('设置')
    }
    .index(this.selectedIndex)
    .onChange((index: number) => {
      this.selectedIndex = index;
    })
    .backgroundColor('#F7FAFC')
  }
}

@Component
struct HomeTab {
  @State private profileName: string = '未选择服务器';
  @State private profileSubtitle: string = '';
  @State private stateText: string = stateLabel(VpnState.Disconnected);
  @State private state: VpnState = VpnState.Disconnected;
  @State private linkState: number = 6;
  @State private stats: VpnStatistics = { txSpeedBytes: 0, rxSpeedBytes: 0, inBytes: 0, outBytes: 0 };
  @State private error: string = '';

  async aboutToAppear(): Promise<void> {
    await this.reloadProfile();
    this.state = vpnController.state;
    this.stateText = stateLabel(this.state);
    this.stats = vpnController.stats;
    this.error = vpnController.lastError;
    vpnController.onState((state: VpnState) => {
      this.state = state;
      this.stateText = stateLabel(state);
      this.linkState = OpenPpp2Native.getLinkState();
    });
    vpnController.onStats((stats: VpnStatistics) => {
      this.stats = stats;
      this.linkState = OpenPpp2Native.getLinkState();
    });
    vpnController.onError((message: string) => {
      this.error = message;
    });
  }

  build() {
    Scroll() {
      Column({ space: 18 }) {
        Row() {
          Column({ space: 4 }) {
            Text('OPENPPP2').fontSize(24).fontWeight(FontWeight.Bold).fontColor('#0F172A')
            Text(this.profileName).fontSize(16).fontColor('#334155')
            Text(this.profileSubtitle).fontSize(12).fontColor('#64748B')
          }
          .alignItems(HorizontalAlign.Start)
          Blank()
          Text(this.stateText)
            .fontSize(14)
            .fontColor(this.state === VpnState.Connected ? '#047857' : '#475569')
            .padding({ left: 12, right: 12, top: 7, bottom: 7 })
            .backgroundColor(this.state === VpnState.Connected ? '#D1FAE5' : '#E2E8F0')
            .borderRadius(16)
        }
        .width('100%')

        Button(this.state === VpnState.Connected || this.state === VpnState.Connecting ? '断开' : '连接')
          .height(64)
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .backgroundColor(this.state === VpnState.Connected || this.state === VpnState.Connecting ? '#B91C1C' : '#0B6B5E')
          .onClick(async () => {
            if (this.state === VpnState.Connected || this.state === VpnState.Connecting) {
              await vpnController.disconnect();
            } else {
              const context = getContext(this) as common.UIAbilityContext;
              const profile = await profileStore.getActiveProfile();
              if (profile === undefined) {
                promptAction.showToast({ message: '请先添加服务器' });
                return;
              }
              await vpnController.connect(context, profile);
            }
          })

        GridRow({ columns: 2, gutter: 12 }) {
          GridCol() {
            StatTile({ label: '上行', value: formatBytes(this.stats.txSpeedBytes) + '/s' })
          }
          GridCol() {
            StatTile({ label: '下行', value: formatBytes(this.stats.rxSpeedBytes) + '/s' })
          }
          GridCol() {
            StatTile({ label: '发送', value: formatBytes(this.stats.outBytes) })
          }
          GridCol() {
            StatTile({ label: '接收', value: formatBytes(this.stats.inBytes) })
          }
        }

        Row() {
          Text(`Link ${this.linkState}`).fontSize(13).fontColor('#64748B')
          Blank()
          Button('刷新')
            .height(34)
            .onClick(() => {
              vpnController.refreshSnapshot();
              this.linkState = OpenPpp2Native.getLinkState();
            })
        }
        .width('100%')

        if (this.error.length > 0) {
          Text(this.error)
            .fontSize(13)
            .fontColor('#991B1B')
            .padding(12)
            .backgroundColor('#FEE2E2')
            .borderRadius(8)
            .width('100%')
        }
      }
      .padding(18)
      .width('100%')
    }
  }

  private async reloadProfile(): Promise<void> {
    const profile = await profileStore.getActiveProfile();
    this.profileName = profile?.name ?? '未选择服务器';
    this.profileSubtitle = profile?.subtitle ?? '';
  }
}

@Component
struct StatTile {
  label: string = '';
  value: string = '';

  build() {
    Column({ space: 8 }) {
      Text(this.label).fontSize(12).fontColor('#64748B')
      Text(this.value).fontSize(18).fontWeight(FontWeight.Medium).fontColor('#0F172A')
    }
    .alignItems(HorizontalAlign.Start)
    .padding(14)
    .width('100%')
    .backgroundColor(Color.White)
    .borderRadius(8)
  }
}

@Component
struct ProfilesTab {
  @State private profiles: ConfigProfile[] = [];
  @State private activeId: string = '';
  @State private subscriptionUrl: string = '';

  async aboutToAppear(): Promise<void> {
    await this.reload();
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        Row() {
          Text('服务器').fontSize(22).fontWeight(FontWeight.Bold).fontColor('#0F172A')
          Blank()
          Button('+')
            .width(42)
            .height(36)
            .onClick(async () => {
              const profile = createDefaultProfile();
              await profileStore.upsert(profile);
              await this.reload();
            })
        }
        .width('100%')

        TextInput({ placeholder: '订阅 URL', text: this.subscriptionUrl })
          .onChange((value: string) => {
            this.subscriptionUrl = value;
          })
        Button('导入订阅')
          .height(42)
          .onClick(async () => {
            if (this.subscriptionUrl.trim().length === 0) {
              return;
            }
            try {
              const result = await new SubscriptionService().fetch(this.subscriptionUrl.trim());
              const count = await profileStore.importSubscription(this.subscriptionUrl.trim(), result);
              promptAction.showToast({ message: `已导入 ${count} 台` });
              await this.reload();
            } catch (err) {
              promptAction.showToast({ message: String(err) });
            }
          })

        ForEach(this.profiles, (profile: ConfigProfile) => {
          Row() {
            Column({ space: 4 }) {
              Text(profile.name).fontSize(16).fontWeight(FontWeight.Medium).fontColor('#0F172A')
              Text(profile.subtitle.length > 0 ? profile.subtitle : '本地配置').fontSize(12).fontColor('#64748B')
            }
            .alignItems(HorizontalAlign.Start)
            Blank()
            Text(profile.id === this.activeId ? '已选' : '选择')
              .fontSize(13)
              .fontColor(profile.id === this.activeId ? '#047857' : '#0B6B5E')
          }
          .padding(14)
          .width('100%')
          .backgroundColor(Color.White)
          .borderRadius(8)
          .onClick(async () => {
            await profileStore.setActiveProfile(profile.id);
            await this.reload();
          })
        }, (profile: ConfigProfile) => profile.id)
      }
      .padding(18)
      .width('100%')
    }
  }

  private async reload(): Promise<void> {
    this.profiles = await profileStore.getProfiles();
    this.activeId = (await profileStore.getActiveProfile())?.id ?? '';
  }
}

@Component
struct OptionsTab {
  @State private profile?: ConfigProfile = undefined;
  @State private tunIp: string = '';
  @State private tunMask: string = '';
  @State private tunPrefix: string = '24';
  @State private route: string = '';
  @State private dns1: string = '';
  @State private dns2: string = '';
  @State private mtu: string = '1400';
  @State private blockQuic: boolean = true;
  @State private proxyOnly: boolean = false;
  @State private allowLan: boolean = false;

  async aboutToAppear(): Promise<void> {
    await this.reload();
  }

  build() {
    Scroll() {
      Column({ space: 12 }) {
        Text(this.profile?.name ?? '参数').fontSize(22).fontWeight(FontWeight.Bold).fontColor('#0F172A')
        TextInput({ placeholder: 'TUN IP', text: this.tunIp }).onChange((value: string) => this.tunIp = value)
        TextInput({ placeholder: 'TUN Mask', text: this.tunMask }).onChange((value: string) => this.tunMask = value)
        TextInput({ placeholder: 'TUN Prefix', text: this.tunPrefix }).onChange((value: string) => this.tunPrefix = value)
        TextInput({ placeholder: 'Route', text: this.route }).onChange((value: string) => this.route = value)
        TextInput({ placeholder: 'DNS 1', text: this.dns1 }).onChange((value: string) => this.dns1 = value)
        TextInput({ placeholder: 'DNS 2', text: this.dns2 }).onChange((value: string) => this.dns2 = value)
        TextInput({ placeholder: 'MTU', text: this.mtu }).onChange((value: string) => this.mtu = value)
        ToggleRow({ label: '阻断 QUIC', checked: this.blockQuic, onChange: (value: boolean) => this.blockQuic = value })
        ToggleRow({ label: 'Proxy Only', checked: this.proxyOnly, onChange: (value: boolean) => this.proxyOnly = value })
        ToggleRow({ label: 'LAN Proxy', checked: this.allowLan, onChange: (value: boolean) => this.allowLan = value })
        Button('保存')
          .height(44)
          .onClick(async () => {
            await this.save();
            promptAction.showToast({ message: '已保存' });
          })
      }
      .padding(18)
      .width('100%')
    }
  }

  private async reload(): Promise<void> {
    this.profile = await profileStore.getActiveProfile();
    const options = cloneOptions(this.profile?.options);
    this.tunIp = options.tunIp;
    this.tunMask = options.tunMask;
    this.tunPrefix = String(options.tunPrefix);
    this.route = options.route;
    this.dns1 = options.dns1;
    this.dns2 = options.dns2;
    this.mtu = String(options.mtu);
    this.blockQuic = options.blockQuic;
    this.proxyOnly = options.proxyOnly;
    this.allowLan = options.allowLan;
  }

  private async save(): Promise<void> {
    if (this.profile === undefined) {
      return;
    }
    const options: LaunchOptions = cloneOptions(this.profile.options);
    options.tunIp = this.tunIp;
    options.tunMask = this.tunMask;
    options.tunPrefix = Number(this.tunPrefix);
    options.route = this.route;
    options.dns1 = this.dns1;
    options.dns2 = this.dns2;
    options.mtu = Number(this.mtu);
    options.blockQuic = this.blockQuic;
    options.proxyOnly = this.proxyOnly;
    options.allowLan = this.allowLan;
    await profileStore.updateOptions(this.profile.id, options);
    await this.reload();
  }
}

@Component
struct ToggleRow {
  label: string = '';
  checked: boolean = false;
  onChange: (value: boolean) => void = () => {};

  build() {
    Row() {
      Text(this.label).fontSize(15).fontColor('#0F172A')
      Blank()
      Toggle({ type: ToggleType.Switch, isOn: this.checked })
        .onChange((value: boolean) => this.onChange(value))
    }
    .padding({ top: 8, bottom: 8 })
    .width('100%')
  }
}

@Component
struct SettingsTab {
  @State private telemetry: TelemetrySettings = {
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
  @State private nativeInfo: string = '';
  @State private lastError: string = '';

  async aboutToAppear(): Promise<void> {
    this.telemetry = await profileStore.getTelemetrySettings();
    this.nativeInfo = OpenPpp2Native.getDefaultCipherSuites();
    this.lastError = OpenPpp2Native.getLastErrorText();
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        Text('设置').fontSize(22).fontWeight(FontWeight.Bold).fontColor('#0F172A')
        ToggleRow({
          label: 'Telemetry',
          checked: this.telemetry.enabled,
          onChange: (value: boolean) => this.telemetry.enabled = value
        })
        TextInput({ placeholder: 'OTLP Endpoint', text: this.telemetry.endpoint })
          .onChange((value: string) => this.telemetry.endpoint = value)
        Button('保存设置')
          .height(42)
          .onClick(async () => {
            await profileStore.setTelemetrySettings(this.telemetry);
            promptAction.showToast({ message: '已保存' });
          })
        Button('Native Smoke')
          .height(42)
          .onClick(() => {
            this.nativeInfo = OpenPpp2Native.getDefaultCipherSuites();
            this.lastError = OpenPpp2Native.getLastErrorText();
          })
        Text(this.nativeInfo)
          .fontSize(12)
          .fontColor('#475569')
          .padding(12)
          .backgroundColor(Color.White)
          .borderRadius(8)
          .width('100%')
        Text(this.lastError)
          .fontSize(12)
          .fontColor('#64748B')
          .padding(12)
          .backgroundColor(Color.White)
          .borderRadius(8)
          .width('100%')
      }
      .padding(18)
      .width('100%')
    }
  }
}
