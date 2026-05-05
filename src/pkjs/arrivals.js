'use strict';

var util = require('./util');

// Pack resolved arrival tuples into a flat byte buffer.
// Wire layout per arrival:
//   [u8 r][u8 g][u8 b]   color
//   lpStr(route)
//   lpStr(headsign)
//   lpStr(time)
//   lpStr(label)         // '' = scheduled; 'Canceled'/'Skipped' = canceled; else live
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

      util.lpStr(bytes, a.r  || '');
      util.lpStr(bytes, routeMeta ? routeMeta.h : '');
      util.lpStr(bytes, a.t  || '');
      util.lpStr(bytes, a.l  || '');
    });

    return bytes;
  },
};
