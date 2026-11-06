(function (root) {
  'use strict';
  const node = typeof module !== 'undefined' && module.exports;
  const packer = node ? require('./resource_pack.js') : root.EPDResourcePack;
  const sources = node ? require('./font_sources.js') : root.EPDFontSources;
  const fields = ['resourceCalendarSource', 'resourceLayoutSource', 'resourceMigrationSource', 'resourceBuildRevision',
    'resourceFontFamily', 'resourceFontPixelSize', 'resourceFontTarget', 'resourceFontMode', 'resourceExtraText'];
  const MAX_FONT = 32 * 1024 * 1024, MAX_PROJECT = 64 * 1024 * 1024;
  function encode64(bytes) {
    let text = '';
    for (let at = 0; at < bytes.length; at += 16384) text += String.fromCharCode(...bytes.subarray(at, at + 16384));
    return root.btoa(text);
  }
  function decode64(value, maximum) {
    const padding = typeof value === 'string' ? value.indexOf('=') : -1;
    if (typeof value !== 'string' || value.length > Math.ceil(maximum / 3) * 4 ||
        value.length % 4 || /[^A-Za-z0-9+/=]/.test(value) ||
        (padding !== -1 && padding !== value.length - 1 && !(padding === value.length - 2 && value.endsWith('=='))))
      throw new Error('工程二进制长度或 Base64 格式无效');
    const bytes = Uint8Array.from(root.atob(value), char => char.charCodeAt(0));
    if (bytes.length > maximum) throw new Error('工程二进制超过容量限制');
    return bytes;
  }
  function values(input) {
    if(input && input.resourceFontPixelSize===undefined) input={...input,resourceFontPixelSize:''};
    if (!input || fields.some(id => typeof input[id] !== 'string' || input[id].length > 2000000))
      throw new Error('工程编辑内容无效');
    const result = Object.fromEntries(fields.map(id => [id, input[id]]));
    if (!['', 'both', '0', '1', '2'].includes(result.resourceFontTarget) ||
        !['', 'supplement', 'redraw'].includes(result.resourceFontMode)) throw new Error('工程字体设置无效');
    return result;
  }
  function encode(snapshot) {
    const result = { format: 'EPD-RESOURCE-PROJECT-2', savedAt: snapshot.savedAt,
      files: snapshot.files.map(file => ({ name: file.name, base64: encode64(file.bytes) })),
      values: values(snapshot.values), bins: (snapshot.bins || []).map(bytes => bytes ? encode64(bytes) : null) };
    if (snapshot.font) {
      if (!(snapshot.font.bytes instanceof Uint8Array) || snapshot.font.bytes.length > MAX_FONT) throw new Error('工程字体超过 32 MiB');
      result.font = { name: snapshot.font.name, base64: encode64(snapshot.font.bytes) };
    }
    return result;
  }
  function decode(project) {
    if (!project || !['EPD-RESOURCE-PROJECT-1', 'EPD-RESOURCE-PROJECT-2'].includes(project.format) ||
        !Array.isArray(project.files) || ![6, 7].includes(project.files.length)) throw new Error('不是完整的设备资源工程');
    const files = project.files.map(file => {
      if (typeof file.name !== 'string' || !file.name || /[\\/]/.test(file.name)) throw new Error('工程文件名无效');
      return { name: file.name, bytes: decode64(file.base64, 524288) };
    });
    const bundle = packer.repairFontBounds(files);
    if (project.format === 'EPD-RESOURCE-PROJECT-1') return { bundle };
    const snapshot = { version: 1, savedAt: project.savedAt, files: bundle.packages.map(item => ({ name: item.name, bytes: item.bytes })),
      values: values(project.values), bins: [] };
    if (!Array.isArray(project.bins) || project.bins.length > 3) throw new Error('工程字模 BIN 列表无效');
    snapshot.bins = project.bins.map(value => value === null ? null : decode64(value, 262144));
    if (project.font) {
      if (typeof project.font.name !== 'string' || !project.font.name || project.font.name.length > 256)
        throw new Error('工程字体名称无效');
      snapshot.font = { name: project.font.name, bytes: decode64(project.font.base64, MAX_FONT) };
      sources.coverage(snapshot.font.bytes);
    }
    if (snapshot.values.resourceFontFamily === 'imported' && !snapshot.font) throw new Error('工程缺少所选的 TTF / OTF 字体');
    return { bundle, snapshot };
  }
  const api = { fields, MAX_PROJECT, encode64, decode64, values, encode, decode };
  if (node) module.exports = api; else root.EPDResourceProject = api;
})(globalThis);
