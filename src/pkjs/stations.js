'use strict';

var util = require('./util');

var CACHE_KEY_DATA = 'nt_stations';
var CACHE_KEY_TS   = 'nt_stations_fetched_at';
var TTL_SECONDS    = 7 * 24 * 3600;

function xhrGet(url, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.timeout = util.XHR_TIMEOUT_MS;
  xhr.onload = function() {
    if (xhr.status >= 200 && xhr.status < 300) {
      try { cb(null, JSON.parse(xhr.responseText)); }
      catch(e) { cb(new Error('parse error'), null); }
    } else {
      cb(new Error('HTTP ' + xhr.status), null);
    }
  };
  xhr.onerror   = function() { cb(new Error('network'), null); };
  xhr.ontimeout = function() { cb(new Error('timeout'), null); };
  xhr.send();
}

// ─── Load (with cache) ────────────────────────────────────────────────────────

module.exports.load = function(workerBase, cb) {
  var raw = localStorage.getItem(CACHE_KEY_DATA);
  var ts  = parseInt(localStorage.getItem(CACHE_KEY_TS) || '0', 10);
  var now = Math.floor(Date.now() / 1000);

  if (raw && (now - ts) < TTL_SECONDS) {
    try { return cb(null, JSON.parse(raw)); } catch(e) { /* corrupt cache, fall through to fetch */ }
  }

  xhrGet(workerBase + '/stations', function(err, data) {
    if (err) { cb(err, null); return; }
    localStorage.setItem(CACHE_KEY_DATA, JSON.stringify(data));
    localStorage.setItem(CACHE_KEY_TS, String(Math.floor(Date.now() / 1000)));
    cb(null, data);
  });
};

module.exports.invalidate = function() {
  localStorage.removeItem(CACHE_KEY_DATA);
  localStorage.removeItem(CACHE_KEY_TS);
};

// ─── Pack /stations JSON into a flat binary blob for watch transfer ────────────
//
// Wire format:
//   [u32 generated_at]
//   [u16 station_count]
//   for each station:
//     [u8 slug_len][bytes slug]
//     [u8 name_len][bytes name]
//     [u8 route_count]
//     for each route:
//       [u8 r][u8 g][u8 b]
//       [u8 route_len][bytes route]
//       [u8 dir]                      ASCII char
//       [u8 hs_len][bytes headsign]   truncated to 24 bytes

module.exports.pack = function(data) {
  if (!data || !data.s) return null;

  var bytes = [];

  // generated_at: big-endian uint32
  var g = data.g | 0;
  bytes.push((g >>> 24) & 0xFF, (g >>> 16) & 0xFF, (g >>> 8) & 0xFF, g & 0xFF);

  var stations = data.s;
  bytes.push((stations.length >>> 8) & 0xFF, stations.length & 0xFF); // uint16

  stations.forEach(function(st) {
    util.lpStr(bytes, st.k, 23);  // slug
    util.lpStr(bytes, st.n, 39);  // name
    var routes = st.r || [];
    bytes.push(routes.length & 0xFF);
    routes.forEach(function(rm) {
      var color = util.hexColor(rm.c);
      bytes.push((color >>> 16) & 0xFF, (color >>> 8) & 0xFF, color & 0xFF);
      util.lpStr(bytes, rm.r, 3);            // route letter
      bytes.push(rm.d.charCodeAt(0) & 0xFF); // dir
      util.lpStr(bytes, rm.h, 24);           // headsign
    });
  });

  var buf  = new ArrayBuffer(bytes.length);
  var view = new Uint8Array(buf);
  for (var i = 0; i < bytes.length; i++) view[i] = bytes[i];
  return buf;
};

