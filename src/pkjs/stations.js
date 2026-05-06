'use strict';

var CACHE_KEY_DATA = 'nt_stations_bin';
var CACHE_KEY_TS   = 'nt_stations_fetched_at';
var TTL_SECONDS    = 7 * 24 * 3600;

// ─── Helpers ──────────────────────────────────────────────────────────────────

var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function arrayToBase64(arr) {
  var res = '', i, len = arr.length;
  for (i = 0; i < len; i += 3) {
    var a = arr[i], b = arr[i+1], c = arr[i+2];
    res += B64[a >> 2];
    res += B64[((a & 3) << 4) | (b >> 4 || 0)];
    res += (i + 1 < len) ? B64[((b & 15) << 2) | (c >> 6 || 0)] : '=';
    res += (i + 2 < len) ? B64[c & 63] : '=';
  }
  return res;
}

function base64ToArray(s) {
  var map = {}; for (var j = 0; j < 64; j++) map[B64[j]] = j;
  var bytes = [];
  for (var i = 0; i < s.length; i += 4) {
    var a = map[s[i]], b = map[s[i+1]], c = map[s[i+2]], d = map[s[i+3]];
    bytes.push((a << 2) | (b >> 4));
    if (s[i+2] !== '=') {
      bytes.push(((b & 15) << 4) | (c >> 2));
      if (s[i+3] !== '=') {
        bytes.push(((c & 3) << 6) | d);
      }
    }
  }
  return bytes;
}

// ─── Load (with cache) ────────────────────────────────────────────────────────

module.exports.load = function(workerBase, cb) {
  var b64 = localStorage.getItem(CACHE_KEY_DATA);
  var tsObj = localStorage.getItem(CACHE_KEY_TS);
  var ts = parseInt(tsObj != null ? '' + tsObj : '0', 10);
  var now = Math.floor(Date.now() / 1000);
  var age = now - ts;

  if (b64 && age < TTL_SECONDS) {
    console.log('[stations] cache hit, age=' + age + 's');
    var bytes = base64ToArray(b64);
    if (bytes.length >= 4) {
      var g = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
      return cb(null, { g: g, bytes: bytes });
    }
  }

  // If no cache or expired, we return error. app.js will handle the fetch.
  // This is a change from previous version where load() did the XHR.
  // However, the plan says app.js handles the fetch now.
  cb(new Error('no cache'), null);
};

module.exports.store = function(bytes) {
  console.log('[stations] storing ' + bytes.length + ' bytes');
  localStorage.setItem(CACHE_KEY_DATA, arrayToBase64(bytes));
  localStorage.setItem(CACHE_KEY_TS, String(Math.floor(Date.now() / 1000)));
};

module.exports.invalidate = function() {
  localStorage.removeItem(CACHE_KEY_DATA);
  localStorage.removeItem(CACHE_KEY_TS);
};
