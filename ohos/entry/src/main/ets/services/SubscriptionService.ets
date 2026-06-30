import { http } from '@kit.NetworkKit';
import { DEFAULT_CONFIG_JSON, RemoteSubscriptionNode, RemoteSubscriptionResult, cloneOptions } from '../models/ProfileModels';

export class SubscriptionService {
  async fetch(url: string): Promise<RemoteSubscriptionResult> {
    const client = http.createHttp();
    try {
      const response = await client.request(url, {
        method: http.RequestMethod.GET,
        connectTimeout: 12000,
        readTimeout: 12000
      });
      return SubscriptionService.parse(String(response.result ?? ''));
    } finally {
      client.destroy();
    }
  }

  static parse(text: string): RemoteSubscriptionResult {
    const root = JSON.parse(text) as Record<string, any>;
    if (String(root['type'] ?? '') !== 'openppp2-subscription') {
      throw new Error('订阅 type 必须是 openppp2-subscription');
    }
    if (Number(root['version']) !== 1) {
      throw new Error('仅支持订阅 version=1');
    }
    const rawNodes = root['nodes'] as Array<Record<string, any>>;
    if (!Array.isArray(rawNodes)) {
      throw new Error('订阅 nodes 必须是数组');
    }

    const prefix = String(root['profilePrefix'] ?? '').trim();
    const nodes: RemoteSubscriptionNode[] = [];
    rawNodes.forEach((node: Record<string, any>) => {
      if (node['enabled'] === false) {
        return;
      }
      const id = String(node['id'] ?? '').trim();
      if (id.length === 0) {
        throw new Error('服务器 id 不能为空');
      }
      const rawName = String(node['name'] ?? id).trim();
      const name = prefix.length === 0 || rawName.startsWith(prefix) ? rawName : `${prefix} ${rawName}`;
      nodes.push({
        id,
        name: name.length === 0 ? id : name,
        subtitle: String(node['subtitle'] ?? ''),
        flag: String(node['flag'] ?? ''),
        json: JSON.stringify(SubscriptionService.buildConfig(node), null, 2),
        options: cloneOptions(node['options'] as Record<string, any>)
      });
    });

    if (nodes.length === 0) {
      throw new Error('订阅中没有可导入服务器');
    }

    return {
      name: String(root['name'] ?? 'OPENPPP2 Subscription'),
      profilePrefix: prefix.length === 0 ? undefined : prefix,
      nodes
    };
  }

  private static buildConfig(node: Record<string, any>): Record<string, any> {
    const config = node['config'];
    if (typeof config === 'string' && config.trim().length > 0) {
      return JSON.parse(config) as Record<string, any>;
    }
    if (config !== null && typeof config === 'object' && !Array.isArray(config)) {
      return config as Record<string, any>;
    }

    const server = String(node['server'] ?? '').trim();
    if (!server.startsWith('ppp://')) {
      throw new Error('server 必须以 ppp:// 开头');
    }
    const root = JSON.parse(DEFAULT_CONFIG_JSON) as Record<string, any>;
    const key = node['key'] as Record<string, any>;
    if (key === null || typeof key !== 'object') {
      throw new Error('精简服务器必须包含 key');
    }

    root['key'] = Object.assign({}, root['key'], key);
    const client = Object.assign({}, root['client'], node['client'] ?? {}) as Record<string, any>;
    client['server'] = server;
    if (node['bandwidth'] !== undefined) {
      client['bandwidth'] = Number(node['bandwidth']);
    }
    client['mappings'] = [];
    root['client'] = client;

    if (node['websocket'] !== undefined) {
      root['websocket'] = Object.assign({}, root['websocket'], node['websocket']);
    }
    return root;
  }
}
