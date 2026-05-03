'use strict';

var util = require('./util');

var STATUS_MAP = { live: 0, scheduled: 1, canceled: 2, skipped: 3, added: 4 };

// Pack resolved arrival tuples into a flat byte buffer.
// Wire layout per arrival:
//   [u8 r][u8 g][u8 b]   color
//   [u8 status]
//   lpStr(route)
//   lpStr(headsign)
//   lpStr(time)
//   lpStr(label)
module.exports = {
  pack: function(arrivals, station, configRoutes) {
    if (!Array.isArray(arrivals)) arrivals = [];
    if (!Array.isArray(configRoutes)) configRoutes = [];

    var routeDir = {};
    configRoutes.forEach(function(entry) {
      var parts = entry.split(':');
      if (parts.length === 2) routeDir[parts[0]] = parts[1];
    });

    var limited = arrivals.slice(0, 10);
    var bytes = [limited.length];

    limited.forEach(function(a) {
      var dir = routeDir[a.r];
      var routeMeta = null;
      (station.r || []).forEach(function(rm) {
        if (!routeMeta && rm.r === a.r && (!dir || rm.d === dir)) routeMeta = rm;
      });

      var color = util.hexColor(routeMeta ? routeMeta.c : null);
      bytes.push((color >>> 16) & 0xFF); // R
      bytes.push((color >>> 8)  & 0xFF); // G
      bytes.push(color & 0xFF);           // B

      bytes.push(STATUS_MAP[a.s] !== undefined ? STATUS_MAP[a.s] : 1);

      util.lpStr(bytes, a.r  || '');
      util.lpStr(bytes, routeMeta ? routeMeta.h : '');
      util.lpStr(bytes, a.t  || '');
      util.lpStr(bytes, a.l  || '');
    });

    var buf = new ArrayBuffer(bytes.length);
    var view = new Uint8Array(buf);
    for (var i = 0; i < bytes.length; i++) view[i] = bytes[i];
    return buf;
  },
};
