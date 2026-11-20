/// Describes the configured package filter, independent of the VPN routes.
String appScopeSummary({
  required bool enabled,
  required String mode,
  required int count,
}) {
  if (!enabled) return '分应用代理已关闭，不限制应用';
  if (count == 0) return '未选择应用，不限制应用';
  return mode == 'deny' ? '排除选中的 $count 个应用' : '仅允许选中的 $count 个应用';
}
