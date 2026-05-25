'use strict';

var stationsModule = require('./stations');

var WORKER_BASE = 'https://nt.sloccy.workers.dev';

var OP = { GET_STATIONS_VERSION: 1, GET_STATIONS_FULL: 2,
           GET_ARRIVALS: 3, REFRESH_STATIONS: 4,
           GET_ALERTS_SUMMARY: 5, GET_ALERT_DETAIL: 6 };
var DATA_TYPE = { STATIONS_VERSION: 1, STATIONS_CHUNK: 2, ARRIVALS: 3,
                  ALERTS_SUMMARY: 4, ALERT_DETAIL: 5,
                  FAVORITES_REQUEST: 6, FAVORITES_LIST: 7, RENAME_FAVORITE: 8 };
var STATUS    = { OK: 0, OFFLINE: 1, NO_DATA: 2, ERROR: 3 };

// Pending callback waiting for a FAVORITES_LIST response from the watch
var s_config_cb = null;

// ─── In-flight serialization queue ────────────────────────────────────────────

var s_in_flight = false;
var s_queue     = [];

function enqueue(fn) {
  s_queue.push(fn);
  if (!s_in_flight) drain();
}

function drain() {
  if (s_queue.length === 0) { s_in_flight = false; console.log('[pkjs] queue drained'); return; }
  s_in_flight = true;
  console.log('[pkjs] drain: ' + s_queue.length + ' item(s) remaining');
  var fn = s_queue.shift();
  fn();
}

// ─── Send helpers ──────────────────────────────────────────────────────────────

var STATUS_NAMES = { 0: 'OK', 1: 'OFFLINE', 2: 'NO_DATA', 3: 'ERROR' };

function sendStatus(queryIndex, code) {
  console.log('[pkjs] sendStatus qi=' + queryIndex + ' code=' + (STATUS_NAMES[code] || code));
  Pebble.sendAppMessage({ STATUS: code, QUERY_INDEX: queryIndex },
    function() { console.log('[pkjs] sendStatus ACK'); },
    function(e) { console.error('[pkjs] sendStatus FAILED: ' + JSON.stringify(e)); });
}

function sendDict(dict, cb) {
  var keys = Object.keys(dict).join(',');
  console.log('[pkjs] sendDict keys=[' + keys + ']');
  Pebble.sendAppMessage(dict,
    function() { console.log('[pkjs] sendDict ACK keys=[' + keys + ']'); cb && cb(); },
    function(e) { console.error('[pkjs] sendDict FAILED keys=[' + keys + ']: ' + JSON.stringify(e)); cb && cb(); });
}

// ─── Stations fetch ───────────────────────────────────────────────────────────

function fetchStations(cb) {
  var url = WORKER_BASE + '/s';
  console.log('[pkjs] stations XHR GET ' + url);
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  try { xhr.setRequestHeader('User-Agent', 'NextTrain/1.1'); } catch (e) {}
  xhr.responseType = 'arraybuffer';
  xhr.timeout = 10000;
  xhr.onload = function() {
    if (xhr.status >= 200 && xhr.status < 300) {
      var bytes = Array.prototype.slice.call(new Uint8Array(xhr.response));
      stationsModule.store(bytes);
      var g = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
      cb(null, { g: g, bytes: bytes });
    } else {
      cb(new Error('HTTP ' + xhr.status), null);
    }
  };
  xhr.onerror = function() { cb(new Error('network'), null); };
  xhr.ontimeout = function() { cb(new Error('timeout'), null); };
  xhr.send();
}

function loadOrFetchStations(cb) {
  stationsModule.load(WORKER_BASE, function(err, data) {
    if (!err && data) return cb(null, data);
    fetchStations(cb);
  });
}

// ─── Stations version ─────────────────────────────────────────────────────────

function handleGetStationsVersion() {
  console.log('[pkjs] handleGetStationsVersion');
  enqueue(function() {
    loadOrFetchStations(function(err, data) {
      if (err) {
        console.error('[pkjs] stations load/fetch error: ' + err.message);
        sendStatus(0, STATUS.OFFLINE); drain(); return;
      }
      console.log('[pkjs] sending STATIONS_VERSION=' + data.g);
      sendDict({ DATA_TYPE: DATA_TYPE.STATIONS_VERSION,
                 STATIONS_VERSION: data.g },
               drain);
    });
  });
}

// ─── Stations full sync ────────────────────────────────────────────────────────

function handleGetStationsFull(inboxSize) {
  var chunkSize = inboxSize > 300 ? inboxSize - 200 : 500;
  console.log('[pkjs] handleGetStationsFull inboxSize=' + inboxSize + ' chunkSize=' + chunkSize);
  enqueue(function() {
    loadOrFetchStations(function(err, data) {
      if (err) {
        console.error('[pkjs] stations load/fetch error: ' + err.message);
        sendStatus(0, STATUS.OFFLINE); drain(); return;
      }
      var blob = data.bytes;
      var total = Math.ceil(blob.length / chunkSize);
      console.log('[pkjs] blob=' + blob.length + 'B, chunks=' + total + ' x ' + chunkSize + 'B');
      var index = 0;

      function sendChunk() {
        if (index >= total) { console.log('[pkjs] all chunks sent'); drain(); return; }
        var start = index * chunkSize;
        var end = Math.min(start + chunkSize, blob.length);
        var slice = blob.slice(start, end);
        var i = index;
        index++;
        console.log('[pkjs] sending chunk ' + (i+1) + '/' + total + ' (' + (end-start) + 'B)');
        sendDict({
          DATA_TYPE:   DATA_TYPE.STATIONS_CHUNK,
          CHUNK_INDEX: i,
          CHUNK_TOTAL: total,
          PAYLOAD:     slice,
        }, sendChunk);
      }
      sendChunk();
    });
  });
}

// ─── Arrivals ─────────────────────────────────────────────────────────────────

function handleGetArrivals(queryIndex, stationSlug, routesStr) {
  console.log('[pkjs] handleGetArrivals qi=' + queryIndex + ' sta=' + stationSlug + ' routes=' + routesStr);
  enqueue(function() {
    loadOrFetchStations(function(err, stationsData) {
      if (err || !stationsData) {
        console.error('[pkjs] arrivals: stations data unavailable');
        sendStatus(queryIndex, STATUS.OFFLINE); drain(); return;
      }

      var url = WORKER_BASE + '/a'
              + '?s=' + encodeURIComponent(stationSlug)
              + '&r=' + encodeURIComponent(routesStr || '');

      var xhr = new XMLHttpRequest();
      xhr.open('GET', url, true);
      try { xhr.setRequestHeader('User-Agent', 'NextTrain/1.1'); } catch (e) {}
      xhr.responseType = 'arraybuffer';
      xhr.timeout = 8000;

      xhr.onload = function() {
        console.log('[pkjs] arrivals xhr status=' + xhr.status);
        if (xhr.status === 404 || xhr.status === 503) {
          sendStatus(queryIndex, STATUS.NO_DATA); drain(); return;
        }
        if (xhr.status < 200 || xhr.status >= 300) {
          sendStatus(queryIndex, STATUS.ERROR); drain(); return;
        }

        var nextRefresh = parseInt(xhr.getResponseHeader('X-Next-Refresh') || '0', 10);
        var resBin = new Uint8Array(xhr.response);
        
        // Decode lean binary: [count] × ([route lpStr][dir u8][time_mins u16][status_s8])
        // Watch format:       [count] × ([r,g,b][route lpStr][headsign lpStr][u16 mins BE][s8 st])
        var pos = 0;
        var count = resBin[pos++];
        var finalBuf = [count];

        // Prepare lookup table from cached stations.bin
        var lookup = {};
        (function() {
          var b = stationsData.bytes;
          var p = 6;
          var sCount = b[4] | (b[5] << 8);
          for (var i = 0; i < sCount; i++) {
            var slen = b[p++];
            var slug = '';
            for (var j = 0; j < slen; j++) slug += String.fromCharCode(b[p++]);
            var rCount = b[p++];
            for (var j = 0; j < rCount; j++) {
              var r = b[p], g = b[p+1], b_ = b[p+2]; p += 3;
              var rlen = b[p++];
              var rName = '';
              for (var k = 0; k < rlen; k++) rName += String.fromCharCode(b[p++]);
              var dir = String.fromCharCode(b[p++]);
              var hlen = b[p++];
              var headsign = '';
              for (var k = 0; k < hlen; k++) headsign += String.fromCharCode(b[p++]);
              
              if (slug === stationSlug) {
                lookup[rName + '.' + dir] = { r: r, g: g, b: b_, h: headsign };
              }
            }
          }
        })();

        for (var i = 0; i < count; i++) {
          var rlen = resBin[pos++];
          var route = '';
          for (var j = 0; j < rlen; j++) route += String.fromCharCode(resBin[pos++]);
          var dirCode = resBin[pos++];
          var dir = String.fromCharCode(dirCode);
          var mins = (resBin[pos++] << 8) | resBin[pos++];
          var status = new Int8Array([resBin[pos++]])[0];

          var atStopLen = resBin[pos++];
          var atStop = '';
          for (var j = 0; j < atStopLen; j++) atStop += String.fromCharCode(resBin[pos++]);

          var static_ = lookup[route + '.' + dir] || { r: 128, g: 128, b: 128, h: '' };

          finalBuf.push(static_.r, static_.g, static_.b);
          lpStrWatch(finalBuf, route, 8);
          lpStrWatch(finalBuf, static_.h, 24);
          finalBuf.push((mins >> 8) & 0xFF, mins & 0xFF);
          finalBuf.push(status & 0xFF);
          lpStrWatch(finalBuf, atStop, 32);
        }

        sendDict({
          DATA_TYPE:    DATA_TYPE.ARRIVALS,
          QUERY_INDEX:  queryIndex,
          NEXT_REFRESH: nextRefresh,
          PAYLOAD:      finalBuf,
        }, drain);
      };

      xhr.onerror   = function() { sendStatus(queryIndex, STATUS.OFFLINE); drain(); };
      xhr.ontimeout = function() { sendStatus(queryIndex, STATUS.OFFLINE); drain(); };
      console.log('[pkjs] arrivals XHR GET ' + url);
      xhr.send();
    });
  });
}

// ─── Alerts summary ───────────────────────────────────────────────────────────

function handleGetAlertsSummary() {
  console.log('[pkjs] handleGetAlertsSummary');
  enqueue(function() {
    var url = WORKER_BASE + '/al';
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.responseType = 'arraybuffer';
    xhr.timeout = 8000;

    xhr.onload = function() {
      if (xhr.status < 200 || xhr.status >= 300) {
        sendStatus(0, STATUS.ERROR); drain(); return;
      }
      var bin = new Uint8Array(xhr.response);
      var pos = 0;
      var routeCount = bin[pos++];
      var payload = [];
      var filteredCount = 0;

      for (var i = 0; i < routeCount; i++) {
        var nlen = bin[pos++];
        var name = '';
        for (var j = 0; j < nlen; j++) name += String.fromCharCode(bin[pos++]);
        var cnt = bin[pos++];
        if (cnt > 0) {
          payload.push(nlen);
          for (var k = 0; k < nlen; k++) payload.push(name.charCodeAt(k) & 0xFF);
          payload.push(cnt);
          filteredCount++;
        }
      }

      payload.unshift(filteredCount);
      sendDict({ DATA_TYPE: DATA_TYPE.ALERTS_SUMMARY, PAYLOAD: payload }, drain);
    };
    xhr.onerror   = function() { sendStatus(0, STATUS.OFFLINE); drain(); };
    xhr.ontimeout = function() { sendStatus(0, STATUS.OFFLINE); drain(); };
    console.log('[pkjs] alerts summary XHR GET ' + url);
    xhr.send();
  });
}

// ─── Alert detail ─────────────────────────────────────────────────────────────

function handleGetAlertDetail(routeName) {
  console.log('[pkjs] handleGetAlertDetail route=' + routeName);
  enqueue(function() {
    var url = WORKER_BASE + '/al?r=' + encodeURIComponent(routeName);
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.responseType = 'arraybuffer';
    xhr.timeout = 8000;

    xhr.onload = function() {
      if (xhr.status === 404) {
        sendDict({ DATA_TYPE: DATA_TYPE.ALERT_DETAIL, PAYLOAD: [0] }, drain); return;
      }
      if (xhr.status < 200 || xhr.status >= 300) {
        sendStatus(0, STATUS.ERROR); drain(); return;
      }
      var bin = new Uint8Array(xhr.response);
      var pos = 0;
      var alertCount = bin[pos++];
      var payload = [Math.min(alertCount, 8)];

      for (var i = 0; i < alertCount && i < 8; i++) {
        pos += 8; // active_from u32 + active_until u32
        pos += 2; // cause u8 + effect u8

        var hlen = bin[pos++];
        var header = '';
        for (var j = 0; j < hlen; j++) header += String.fromCharCode(bin[pos++]);

        var dlen = bin[pos++] | (bin[pos++] << 8);
        var desc = '';
        for (var j = 0; j < dlen; j++) desc += String.fromCharCode(bin[pos++]);

        // Truncate and encode as lpStr (u8 len + bytes)
        var hb = [];
        for (var j = 0; j < Math.min(header.length, 80); j++) hb.push(header.charCodeAt(j) & 0xFF);
        payload.push(hb.length);
        for (var j = 0; j < hb.length; j++) payload.push(hb[j]);

        var db = [];
        for (var j = 0; j < Math.min(desc.length, 160); j++) db.push(desc.charCodeAt(j) & 0xFF);
        payload.push(db.length);
        for (var j = 0; j < db.length; j++) payload.push(db[j]);
      }

      sendDict({ DATA_TYPE: DATA_TYPE.ALERT_DETAIL, PAYLOAD: payload }, drain);
    };
    xhr.onerror   = function() { sendStatus(0, STATUS.OFFLINE); drain(); };
    xhr.ontimeout = function() { sendStatus(0, STATUS.OFFLINE); drain(); };
    console.log('[pkjs] alert detail XHR GET ' + url);
    xhr.send();
  });
}

function lpStrWatch(bytes, s, maxLen) {
  var str = (s || '').slice(0, maxLen);
  bytes.push(str.length);
  for (var i = 0; i < str.length; i++) bytes.push(str.charCodeAt(i) & 0xFF);
}

// ─── Refresh stations ─────────────────────────────────────────────────────────

function handleRefreshStations() {
  console.log('[pkjs] handleRefreshStations: invalidating cache');
  enqueue(function() {
    stationsModule.invalidate();
    fetchStations(function(err, data) {
      if (err) {
        console.error('[pkjs] refresh: stations fetch error: ' + err.message);
        sendStatus(0, STATUS.OFFLINE); drain(); return;
      }
      console.log('[pkjs] refresh: fetch OK, sending version=0 to force re-sync');
      sendDict({ DATA_TYPE: DATA_TYPE.STATIONS_VERSION,
                 STATIONS_VERSION: 0 },
               drain);
    });
  });
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

Pebble.addEventListener('ready', function() {
  console.log('[pkjs] ready');
  loadOrFetchStations(function(err, data) {
    if (err) {
      console.warn('[pkjs] stations load/fetch failed: ' + err.message);
      return;
    }
    console.log('[pkjs] pre-warm OK, proactively sending STATIONS_VERSION=' + data.g);
    sendDict({ DATA_TYPE: DATA_TYPE.STATIONS_VERSION, STATIONS_VERSION: data.g }, null);
  });
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload.DATA_TYPE === DATA_TYPE.FAVORITES_LIST) {
    if (s_config_cb) {
      var cb = s_config_cb;
      s_config_cb = null;
      cb(e.payload.PAYLOAD || '[]');
    }
    return;
  }

  var op           = e.payload.OP;
  var queryIndex   = e.payload.QUERY_INDEX !== undefined ? e.payload.QUERY_INDEX : 0;
  var stationSlug  = e.payload.QUERY_STATION || '';
  var routesStr    = e.payload.QUERY_ROUTES  || '';

  console.log('[pkjs] op=' + op + ' idx=' + queryIndex + ' sta=' + stationSlug);

  switch (op) {
    case OP.GET_STATIONS_VERSION: handleGetStationsVersion();                             break;
    case OP.GET_STATIONS_FULL:    handleGetStationsFull(e.payload.INBOX_SIZE || 0);      break;
    case OP.GET_ARRIVALS:         handleGetArrivals(queryIndex, stationSlug, routesStr); break;
    case OP.REFRESH_STATIONS:     handleRefreshStations();                                break;
    case OP.GET_ALERTS_SUMMARY:   handleGetAlertsSummary();                               break;
    case OP.GET_ALERT_DETAIL:     handleGetAlertDetail(routesStr);                        break;
    default: console.warn('[pkjs] unknown op: ' + op);
  }
});

Pebble.addEventListener('showConfiguration', function() {
  function openConfig(favJson) {
    var url = WORKER_BASE + '/config.html?favs=' + encodeURIComponent(favJson || '[]');
    Pebble.openURL(url);
  }
  s_config_cb = openConfig;
  sendDict({ DATA_TYPE: DATA_TYPE.FAVORITES_REQUEST }, null);
  setTimeout(function() {
    if (s_config_cb) {
      var cb = s_config_cb;
      s_config_cb = null;
      cb('[]');
    }
  }, 4000);
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) return;
  var changes;
  try {
    changes = JSON.parse(decodeURIComponent(e.response));
  } catch(err) {
    return;
  }
  if (!Array.isArray(changes) || changes.length === 0) return;

  function next(k) {
    if (k >= changes.length) return;
    var c = changes[k];
    sendDict({
      DATA_TYPE:    DATA_TYPE.RENAME_FAVORITE,
      RENAME_INDEX: c.i | 0,
      RENAME_NAME:  c.n || ''
    }, function() { next(k + 1); });
  }
  next(0);
});
