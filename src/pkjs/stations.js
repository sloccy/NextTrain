'use strict';

var CACHE_KEY_DATA = 'nt_stations_bin';
var CACHE_KEY_TS   = 'nt_stations_fetched_at';
var TTL_SECONDS    = 7 * 24 * 3600;

// ─── Helpers ──────────────────────────────────────────────────────────────────

function arrayToBase64(arr) {
  var binary = '';
  for (var i = 0; i < arr.length; i++) binary += String.fromCharCode(arr[i]);
  return btoa(binary);
}

function base64ToArray(base64) {
  var binary = atob(base64);
  var arr = [];
  for (var i = 0; i < binary.length; i++) arr.push(binary.charCodeAt(i));
  return arr;
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
      var g = ((bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3]) >>> 0;
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
