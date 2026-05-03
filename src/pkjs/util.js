'use strict';

var XHR_TIMEOUT_MS = 8000;

// Length-prefixed ASCII string: [u8 len][bytes], truncated to maxLen (default 63)
function lpStr(bytes, s, maxLen) {
  var limit = maxLen !== undefined ? maxLen : 63;
  var str = (s || '').slice(0, limit);
  bytes.push(str.length);
  for (var i = 0; i < str.length; i++) bytes.push(str.charCodeAt(i) & 0xFF);
}

// Parse a CSS hex color string (#RRGGBB) to an integer, with 0x888888 fallback
function hexColor(c) {
  var raw = c ? parseInt(c.replace('#', ''), 16) : NaN;
  return isNaN(raw) ? 0x888888 : raw;
}

module.exports = { lpStr: lpStr, hexColor: hexColor, XHR_TIMEOUT_MS: XHR_TIMEOUT_MS };
