'use strict';

var stationsModule = require('./stations');
var arrivalsModule = require('./arrivals');

var WORKER_BASE = 'https://nexttrainworker.sloccy.workers.dev';

var OP = { GET_STATIONS_VERSION: 1, GET_STATIONS_FULL: 2,
           GET_ARRIVALS: 3, REFRESH_STATIONS: 4 };
var DATA_TYPE = { STATIONS_VERSION: 1, STATIONS_CHUNK: 2, ARRIVALS: 3,
                  FAVORITES_REQUEST: 6, FAVORITES_LIST: 7, RENAME_FAVORITE: 8 };
var STATUS    = { OK: 0, OFFLINE: 1, NO_DATA: 2, ERROR: 3 };

// Pending callback waiting for a FAVORITES_LIST response from the watch
var s_config_cb = null;

// ─── In-flight serialization queue ────────────────────────────────────────────
// Phone serializes its own outbound sends via the success-callback chain.
// We also need to guard against overlapping XHR requests when the watch
// fires multiple OP_GET_ARRIVALS in quick succession (background prefetch).

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

// ─── Stations version ─────────────────────────────────────────────────────────

function handleGetStationsVersion() {
  console.log('[pkjs] handleGetStationsVersion');
  enqueue(function() {
    stationsModule.load(WORKER_BASE, function(err, data) {
      if (err) {
        console.error('[pkjs] stations load error: ' + err.message);
        sendStatus(0, STATUS.OFFLINE); drain(); return;
      }
      var version = data.g | 0;
      console.log('[pkjs] sending STATIONS_VERSION=' + version);
      sendDict({ DATA_TYPE: DATA_TYPE.STATIONS_VERSION,
                 STATIONS_VERSION: version },
               drain);
    });
  });
}

// ─── Stations full sync ────────────────────────────────────────────────────────

function handleGetStationsFull(inboxSize) {
  // Reserve 200B headroom for AppMessage transport+dict overhead. 50B was too
  // tight: phone sent 4046B chunks into a 4096B inbox and they never arrived.
  // The previous proven config was 700B inbox / 500B chunks, also 200B slack.
  var chunkSize = inboxSize > 300 ? inboxSize - 200 : 500;
  console.log('[pkjs] handleGetStationsFull inboxSize=' + inboxSize + ' chunkSize=' + chunkSize);
  enqueue(function() {
    stationsModule.load(WORKER_BASE, function(err, data) {
      if (err) {
        console.error('[pkjs] stations load error: ' + err.message);
        sendStatus(0, STATUS.OFFLINE); drain(); return;
      }
      var blob = stationsModule.pack(data);
      if (!blob) {
        console.error('[pkjs] stations pack returned null');
        sendStatus(0, STATUS.ERROR); drain(); return;
      }
      var total  = Math.ceil(blob.length / chunkSize);
      console.log('[pkjs] blob=' + blob.length + 'B, chunks=' + total + ' x ' + chunkSize + 'B');
      var index  = 0;

      function sendChunk() {
        if (index >= total) { console.log('[pkjs] all chunks sent'); drain(); return; }
        var start   = index * chunkSize;
        var end     = Math.min(start + chunkSize, blob.length);
        var slice   = blob.slice(start, end);
        var i       = index;
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
    stationsModule.load(WORKER_BASE, function(err, stationsData) {
      if (err || !stationsData) {
        console.error('[pkjs] arrivals: stations load error: ' + (err ? err.message : 'null data'));
        sendStatus(queryIndex, STATUS.OFFLINE); drain(); return;
      }

      var station = null;
      (stationsData.s || []).forEach(function(s) {
        if (s.k === stationSlug) station = s;
      });
      if (!station) {
        console.error('[pkjs] arrivals: station not found: ' + stationSlug);
        sendStatus(queryIndex, STATUS.NO_DATA); drain(); return;
      }

      var url = WORKER_BASE + '/arrivals'
              + '?station=' + encodeURIComponent(stationSlug)
              + '&routes='  + encodeURIComponent(routesStr || '');

      var xhr = new XMLHttpRequest();
      xhr.open('GET', url, true);
      xhr.timeout = 8000;

      xhr.onload = function() {
        var bodyStr = xhr.responseText ? '' + xhr.responseText : '';
        console.log('[pkjs] arrivals xhr status=' + xhr.status + ' len=' + bodyStr.length);
        // Worker uses 404 to mean "no arrivals match this station/route/direction" — not a hard error.
        if (xhr.status === 404 || xhr.status === 503) {
          console.log('[pkjs] arrivals ' + xhr.status + ' → NO_DATA, body: ' + bodyStr.slice(0, 200));
          sendStatus(queryIndex, STATUS.NO_DATA); drain(); return;
        }
        if (xhr.status < 200 || xhr.status >= 300) {
          console.error('[pkjs] arrivals HTTP ' + xhr.status + ' body: ' + bodyStr.slice(0, 200));
          sendStatus(queryIndex, STATUS.ERROR); drain(); return;
        }

        var body;
        try {
          // '' + coerces STPyV8 JSObject → JS string before JSON.parse (same fix as stations.js)
          body = JSON.parse(bodyStr);
        } catch(e) {
          console.error('[pkjs] arrivals JSON parse error: ' + e.message + ' body: ' + bodyStr.slice(0, 200));
          sendStatus(queryIndex, STATUS.ERROR); drain(); return;
        }

        var routes = (routesStr || '').split(',');
        var buf = arrivalsModule.pack(body.a || [], station, routes);

        sendDict({
          DATA_TYPE:    DATA_TYPE.ARRIVALS,
          QUERY_INDEX:  queryIndex,
          NEXT_REFRESH: (body.n | 0),
          PAYLOAD:      buf,
        }, drain);
      };

      xhr.onerror   = function() { sendStatus(queryIndex, STATUS.OFFLINE); drain(); };
      xhr.ontimeout = function() { sendStatus(queryIndex, STATUS.OFFLINE); drain(); };
      console.log('[pkjs] arrivals XHR GET ' + url);
      xhr.send();
    });
  });
}

// ─── Refresh stations ─────────────────────────────────────────────────────────

function handleRefreshStations() {
  console.log('[pkjs] handleRefreshStations: invalidating cache');
  enqueue(function() {
    stationsModule.invalidate();
    stationsModule.load(WORKER_BASE, function(err, data) {
      if (err) {
        console.error('[pkjs] refresh: stations load error: ' + err.message);
        sendStatus(0, STATUS.OFFLINE); drain(); return;
      }
      console.log('[pkjs] refresh: load OK, station_count=' + ((data && data.s) ? data.s.length : 0) + ', sending version=0 to force re-sync');
      sendDict({ DATA_TYPE: DATA_TYPE.STATIONS_VERSION,
                 STATIONS_VERSION: 0 },
               drain);
    });
  });
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

Pebble.addEventListener('ready', function() {
  console.log('[pkjs] ready');
  // Pre-warm cache and proactively send version — watch's op=1 may arrive before
  // the appmessage listener is registered and get silently dropped by the OS.
  stationsModule.load(WORKER_BASE, function(err, data) {
    if (err) {
      console.warn('[pkjs] stations pre-warm failed: ' + err.message);
      return;
    }
    var version = data.g | 0;
    console.log('[pkjs] pre-warm OK, proactively sending STATIONS_VERSION=' + version);
    sendDict({ DATA_TYPE: DATA_TYPE.STATIONS_VERSION, STATIONS_VERSION: version }, null);
  });
});

Pebble.addEventListener('appmessage', function(e) {
  // Watch → phone responses for the config flow
  if (e.payload.DATA_TYPE === DATA_TYPE.FAVORITES_LIST) {
    console.log('[pkjs] received FAVORITES_LIST');
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
    case OP.GET_STATIONS_VERSION: handleGetStationsVersion();                          break;
    case OP.GET_STATIONS_FULL:    handleGetStationsFull(e.payload.INBOX_SIZE || 0);   break;
    case OP.GET_ARRIVALS:         handleGetArrivals(queryIndex, stationSlug, routesStr); break;
    case OP.REFRESH_STATIONS:     handleRefreshStations();                             break;
    default: console.warn('[pkjs] unknown op: ' + op);
  }
});

Pebble.addEventListener('showConfiguration', function() {
  console.log('[pkjs] showConfiguration');

  function openConfig(favJson) {
    var url = WORKER_BASE + '/config.html?favs=' + encodeURIComponent(favJson || '[]');
    console.log('[pkjs] openURL ' + url);
    Pebble.openURL(url);
  }

  // Ask watch for current favorites; fall back to empty list after 4s
  s_config_cb = openConfig;
  sendDict({ DATA_TYPE: DATA_TYPE.FAVORITES_REQUEST }, null);
  setTimeout(function() {
    if (s_config_cb) {
      console.warn('[pkjs] showConfiguration: watch timeout, opening with empty list');
      var cb = s_config_cb;
      s_config_cb = null;
      cb('[]');
    }
  }, 4000);
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) { console.log('[pkjs] config: cancelled'); return; }
  var changes;
  try {
    changes = JSON.parse(decodeURIComponent(e.response));
  } catch(err) {
    console.error('[pkjs] config parse error: ' + err.message);
    return;
  }
  if (!Array.isArray(changes) || changes.length === 0) return;

  // Send one RENAME_FAVORITE message per changed entry, sequenced via callbacks
  function next(k) {
    if (k >= changes.length) { console.log('[pkjs] config: all renames sent'); return; }
    var c = changes[k];
    console.log('[pkjs] config: renaming fav ' + c.i + ' to "' + c.n + '"');
    sendDict({
      DATA_TYPE:    DATA_TYPE.RENAME_FAVORITE,
      RENAME_INDEX: c.i | 0,
      RENAME_NAME:  c.n || ''
    }, function() { next(k + 1); });
  }
  next(0);
});
