import QtQuick 2.6
import QtPositioning 5.0

// A self-contained OSM "slippy map": plain QtQuick Image tiles laid out in a
// grid, centred on the live GPS position. Needs NO QtLocation geoservices
// plugin — on Sailfish (QtLocation 5.4.2) the osm plugin is absent, so the
// normal Map renders blank. QtPositioning (GPS) is available independently, so
// this works on-device. CarMain loads it when the real QtLocation map can't
// produce tiles.
Item {
    id: tm
    clip: true
    function mlog(s) { if (typeof carScreen !== "undefined" && carScreen) carScreen.log("map: " + s); else console.log("map: " + s) }
    property bool _tileErrLogged: false
    property bool _trafficErrLogged: false
    property bool _fixLogged: false
    Component.onCompleted: mlog("TileMap loaded; host=" + tileHost + " tiles=" + _tiles.length)

    // Centre of the view. Defaults to Helsinki until GPS gets a fix. This is the
    // DISPLAY centre: it animates smoothly between the 1 Hz GPS fixes so the map
    // pans fluidly instead of jumping once a second (the "laggy" feel).
    property real centerLat: 60.1699
    property real centerLon: 24.9384
    property bool _animatePan: false // off for the first fix (snap, don't fly from Helsinki)
    Behavior on centerLat { enabled: tm._animatePan; NumberAnimation { duration: 950; easing.type: Easing.Linear } }
    Behavior on centerLon { enabled: tm._animatePan; NumberAnimation { duration: 950; easing.type: Easing.Linear } }
    // Tile/render ANCHOR (snapped). Tiles are built against this and only rebuilt
    // when the car has moved ~a tile; between rebuilds the tile layer just
    // translates by the sub-anchor offset (_panX/_panY) — cheap and smooth.
    property real _rLat: 60.1699
    property real _rLon: 24.9384
    onZoomChanged: { _rLat = centerLat; _rLon = centerLon } // re-anchor tiles on zoom
    // Wide while we wait for a fix (so the "searching" area is visible), then
    // snap to a street-level driving zoom once we actually know where we are.
    property int zoom: 15
    property int drivingZoom: 17
    readonly property int tileSize: 256
    property bool night: (typeof carScreen !== "undefined" && carScreen) ? carScreen.nightMode : false
    property string tileHost: (typeof osmTileHost !== "undefined" && osmTileHost)
                              ? osmTileHost : "https://tile.openstreetmap.org/"
    property bool follow: true   // keep the view centred on GPS

    // Orientation: false = north-up (fixed), true = heading-up (the map rotates
    // with the car so "up" is the direction of travel, like Google Maps). The
    // compass button in CarMain toggles carScreen.mapHeadingUp (persisted by the
    // Controller, so the choice survives restarts); _dispHeading is the heading used.
    property bool headingUp: (typeof carScreen !== "undefined" && carScreen) ? carScreen.mapHeadingUp : false
    property real _dispHeading: 0
    // Current speed in km/h from the GPS fix (-1 until known / when invalid), so
    // CarMain can show a Google-Maps-style speedometer in the corner.
    property int speedKmh: -1
    // Posted speed limit of the road we're on (km/h), -1 when unknown. From the
    // OSM maxspeed tag of the nearest road via Nominatim reverse geocoding,
    // throttled (one lookup per >= 8 s and >= 120 m moved; usage policy allows
    // 1/s). While navigating we look up the route point nearest to us, so a side
    // street next to the road doesn't win.
    property int speedLimit: -1
    property var _limitAt: null
    property double _limitTime: 0
    property bool _limitBusy: false
    property int _limitMisses: 0
    function _checkSpeedLimit(here) {
        if (_limitBusy) return
        var now = Date.now()
        if (_limitAt && (now - _limitTime < 8000 || here.distanceTo(_limitAt) < 120)) return
        var q = here
        if (navigating && _routeCoords.length) {
            var best = -1, c = _routeCoords
            for (var i = 0; i < c.length; i++) {
                var v = QtPositioning.coordinate(c[i][1], c[i][0]), d = here.distanceTo(v)
                if (best < 0 || d < best) { best = d; q = v }
            }
            if (best > offRouteM) q = here   // off the route: use where we are
        }
        _limitBusy = true; _limitAt = here; _limitTime = now
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            _limitBusy = false
            if (xhr.status !== 200) return
            try {
                var r = JSON.parse(xhr.responseText)
                // Ignore paths the car can't be on (keep the last limit).
                if (r.category !== "highway" || /^(footway|cycleway|path|pedestrian|steps|track|bridleway|corridor)$/.test(r.type))
                    return
                var lim = _parseMaxspeed((r.extratags || {}).maxspeed)
                if (lim > 0) { _limitMisses = 0; speedLimit = lim }
                else if (++_limitMisses >= 2) speedLimit = -1  // road has no tag: don't show a stale limit
            } catch (e) { }
        }
        xhr.open("GET", "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=17&extratags=1&layer=address"
                 + "&lat=" + q.latitude.toFixed(6) + "&lon=" + q.longitude.toFixed(6))
        xhr.send()
    }
    // OSM maxspeed: "50", "50 mph", "50;30" (take the first). Implicit zone
    // values ("FI:urban") and "none"/"signals" give -1 (not shown).
    function _parseMaxspeed(v) {
        if (!v) return -1
        var m = /^\s*([0-9]+)\s*(mph|knots)?/i.exec(String(v).split(";")[0])
        if (!m) return -1
        var n = parseInt(m[1])
        if (m[2] && m[2].toLowerCase() === "mph") n = Math.round(n * 1.609344)
        return (n > 0 && n < 200) ? n : -1
    }
    // Tiles/route/markers live on a SQUARE canvas sized to the viewport diagonal
    // so that, when rotated, its corners still cover the whole visible area.
    readonly property real _canvas: Math.ceil(Math.sqrt(width * width + height * height))

    // --- live traffic overlay (TomTom flow tiles) ---
    // Keyless OSM has no traffic data, so this needs a user-supplied TomTom key
    // (phone settings). When present and enabled, coloured traffic-flow tiles are
    // laid over the base map for exactly the area on screen (which follows the car).
    property string trafficKey: (typeof carScreen !== "undefined" && carScreen) ? carScreen.trafficKey : ""
    property bool trafficOn: (typeof carScreen !== "undefined" && carScreen) ? carScreen.trafficEnabled : false
    function trafficUrl(z, x, y) {
        return "https://api.tomtom.com/traffic/map/4/tile/flow/relative/"
             + z + "/" + x + "/" + y + ".png?key=" + trafficKey
    }

    // --- navigation API (consumed by CarMain via navMap) ---
    readonly property bool usable: true
    property bool searching: false
    property var geocodeModel: []            // [{address:{text}, coordinate}]
    property bool navigating: false
    property string destinationLabel: ""
    property string summary: ""              // "12 km · 18 min"
    property string nextInstruction: ""      // e.g. "Turn right onto Main St"
    property string nextDistance: ""         // e.g. "300 m"
    property int maneuverDir: 0              // QtLocation RouteManeuver.Direction-style
    property int remainingM: 0
    property int remainingS: 0
    property string etaText: ""
    // Auto-end navigation once we're on the final leg and within this many metres
    // of the destination, so the user doesn't have to stop it by hand.
    property int arrivalM: 35

    // Estimated battery charge on arrival (%), for an electric car, computed from
    // the car's reported state-of-charge and remaining range vs. the route's
    // remaining distance (the car's range already embeds its consumption model):
    //   arrival% = SoC% * (range - distanceToGo) / range
    // -1 when not an EV / no car energy data / not navigating. Reactive on the
    // carScreen energy properties and the live remaining distance.
    readonly property int evArrivalPct: {
        if (typeof carScreen === "undefined" || !carScreen || !carScreen.evElectric)
            return -1
        if (!navigating || carScreen.evRange <= 0 || carScreen.evLevel < 0)
            return -1
        var remKm = remainingM / 1000.0
        var pct = Math.round(carScreen.evLevel * (carScreen.evRange - remKm) / carScreen.evRange)
        return Math.max(0, Math.min(100, pct))
    }
    // route internals
    property var _routeCoords: []            // [[lon,lat], ...] drawn polyline
    property var _steps: []                   // [{text,dir,lat,lon,dist}]
    property int _curStep: 0
    property var _destCoord: null
    property int _totalM: 0                    // whole-route distance/time, for live
    property int _totalS: 0                    // remaining estimates as we progress
    property int _offCount: 0                  // consecutive off-route GPS samples
    property double _lastReroute: 0            // ms timestamp of the last reroute
    property bool _rerouting: false            // a reroute request is in flight
    readonly property int offRouteM: 45        // metres off the line that counts as off-route

    // --- geocoding (Nominatim) + routing (OSRM) over plain HTTP. QtLocation's
    // osm plugin fails to load on-device, so we talk to the public OSM services
    // directly; the engine's NAM factory stamps the required User-Agent. ---
    function search(text) {
        if (!text || text.length < 2) { geocodeModel = []; return }
        searching = true
        // Bias toward where we are: a viewbox around the current position makes
        // Nominatim prefer nearby places (a preference, not a hard bound, so a
        // deliberately far destination still shows up), and we then re-sort the
        // results by distance so a nearby match beats a far-away namesake
        // instead of some street thousands of km away ranking first.
        var here = (gps.position.latitudeValid && gps.position.longitudeValid)
                   ? gps.position.coordinate : QtPositioning.coordinate(centerLat, centerLon)
        var d = 2.0 // ~200 km half-box around us
        var vb = (here.longitude - d) + "," + (here.latitude + d) + ","
               + (here.longitude + d) + "," + (here.latitude - d)
        var url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=10"
                + "&viewbox=" + vb + "&q=" + encodeURIComponent(text)
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            searching = false
            if (xhr.status !== 200) { mlog("geocode http " + xhr.status); geocodeModel = []; return }
            try {
                var arr = JSON.parse(xhr.responseText), out = []
                for (var i = 0; i < arr.length; i++) {
                    var c = QtPositioning.coordinate(parseFloat(arr[i].lat), parseFloat(arr[i].lon))
                    out.push({ address: { text: arr[i].display_name }, coordinate: c,
                               _dist: here.distanceTo(c), _imp: arr[i].importance || 0 })
                }
                // Nearest first; Nominatim's relevance (importance) breaks ties.
                out.sort(function(a, b) { return (a._dist - b._dist) || (b._imp - a._imp) })
                geocodeModel = out
                var near = out.length ? Math.round(out[0]._dist / 1000) : 0
                mlog("geocode '" + text + "' -> " + out.length + " (nearest " + near + " km)")
            } catch (e) { mlog("geocode parse err " + e); geocodeModel = [] }
        }
        xhr.open("GET", url); xhr.send()
    }

    // --- EV charging stations nearby (OpenStreetMap via Nominatim; no key) ---
    // [{address:{text}, sub, coordinate, _dist, kw}], nearest first. Nominatim
    // returns at most 40 hits per box in no distance order, so look ~5 km around
    // first and widen to chargerRadiusKm only when that finds few. (Overpass
    // would be the natural API, but its public servers routinely take 40 s+.)
    property var chargerModel: []
    property bool chargerSearching: false
    readonly property int chargerRadiusKm: 25
    property var _chargerHere: null
    property var _chargerSeen: ({})
    function findChargers() {
        _chargerHere = (gps.position.latitudeValid && gps.position.longitudeValid)
                       ? gps.position.coordinate : QtPositioning.coordinate(centerLat, centerLon)
        chargerModel = []
        _chargerSeen = {}
        chargerSearching = true
        _chargerQuery(5)
    }
    Timer {   // Nominatim usage policy: at most one request per second
        id: chargerWiden; interval: 1100
        onTriggered: _chargerQuery(chargerRadiusKm)
    }
    function _chargerQuery(km) {
        var here = _chargerHere
        var dLat = km / 111.0, dLon = km / (111.0 * Math.max(0.2, Math.cos(here.latitude * Math.PI / 180)))
        var vb = (here.longitude - dLon).toFixed(4) + "," + (here.latitude + dLat).toFixed(4) + ","
               + (here.longitude + dLon).toFixed(4) + "," + (here.latitude - dLat).toFixed(4)
        var url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=40&bounded=1"
                + "&extratags=1&amenity=charging_station&viewbox=" + vb
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var out = chargerModel.slice(0)
            if (xhr.status === 200) {
                try {
                    var arr = JSON.parse(xhr.responseText)
                    for (var i = 0; i < arr.length; i++) {
                        var r = arr[i], key = r.osm_type + r.osm_id
                        if (_chargerSeen[key]) continue
                        _chargerSeen[key] = true
                        var t = r.extratags || {}
                        var c = QtPositioning.coordinate(parseFloat(r.lat), parseFloat(r.lon))
                        var who = t.operator || t.network || t.brand || ""
                        var title = r.name || who || "Charging station"
                        var kw = _chargerKw(t), bits = []
                        if (kw > 0) bits.push((kw >= 10 ? Math.round(kw) : kw.toFixed(1)) + " kW")
                        if (t.capacity) bits.push(t.capacity + (t.capacity === "1" ? " point" : " points"))
                        if (who && who !== title) bits.push(who)
                        // No details at all: show where it is (street, area).
                        if (!bits.length && r.display_name)
                            bits.push(r.display_name.split(",").slice(r.name ? 1 : 0, 3).join(",").trim())
                        out.push({ address: { text: title }, sub: bits.join(" · "), coordinate: c,
                                   _dist: here.distanceTo(c), kw: kw })
                    }
                } catch (err) { mlog("chargers parse err " + err) }
            } else {
                mlog("chargers http " + xhr.status)
            }
            out.sort(function(a, b) { return a._dist - b._dist })
            chargerModel = out.slice(0, 30)
            if (km < chargerRadiusKm && out.length < 8) {
                chargerWiden.start()
            } else {
                chargerSearching = false
                mlog("chargers within " + km + " km: " + out.length)
            }
        }
        xhr.open("GET", url); xhr.send()
    }
    // Highest charging power (kW) from the OSM tags: socket:<type>:output or
    // charging_station:output, e.g. "150 kW", "11+22kW", "22 kW;50 kW", "11000 W".
    function _chargerKw(t) {
        var best = 0
        for (var k in t) {
            if (!/(^socket:.*:output$)|(^charging_station:output$)/.test(k)) continue
            var re = /([0-9]+(?:[.,][0-9]+)?)\s*(kW|kVA|W)?/gi, m
            while ((m = re.exec(String(t[k]))) !== null) {
                var v = parseFloat(m[1].replace(",", "."))
                if ((m[2] && m[2].toLowerCase() === "w") || v >= 1000) v /= 1000
                if (v > best && v < 1000) best = v
            }
        }
        return best
    }

    function navigateTo(coord, label) {
        if (!coord || !coord.isValid) return
        var from = (gps.position.latitudeValid && gps.position.longitudeValid)
                   ? gps.position.coordinate : QtPositioning.coordinate(centerLat, centerLon)
        destinationLabel = label || ""
        _destCoord = coord
        _offCount = 0
        summary = "Routing…"
        _requestRoute(from, coord, _headingNow())
    }

    // Current travel heading (degrees, 0=N) when GPS reports one, else -1.
    function _headingNow() {
        return gps.position.directionValid ? gps.position.direction : -1
    }

    // Shared OSRM request used by the initial route and by re-routing. `bearing`
    // (>=0) is our heading: OSRM then snaps the start to the road we are actually
    // travelling on, in our direction — not merely the nearest road. Without it,
    // just after leaving the planned road OSRM snaps back onto it and keeps
    // routing us to the original route instead of the shortest path from here.
    function _requestRoute(from, coord, bearing) {
        var useBearing = (bearing !== undefined && bearing >= 0)
        var url = "https://router.project-osrm.org/route/v1/driving/"
                + from.longitude + "," + from.latitude + ";" + coord.longitude + "," + coord.latitude
                + "?overview=full&geometries=geojson&steps=true&continue_straight=false"
        if (useBearing)
            url += "&bearings=" + Math.round(bearing) + ",90;" // start heading ±90°, destination free
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            var data = null
            if (xhr.status === 200) { try { data = JSON.parse(xhr.responseText) } catch (e) { data = null } }
            var haveRoute = data && data.routes && data.routes.length > 0
            if (!haveRoute && useBearing) {
                // No road matched our heading (noisy GPS / odd geometry): retry
                // unconstrained rather than fail.
                mlog("route: no segment for heading, retrying without bearing")
                _requestRoute(from, coord, -1)
                return
            }
            _rerouting = false
            if (!haveRoute) { summary = "No route"; mlog("route http " + xhr.status); return }
            _applyRoute(data)
        }
        xhr.open("GET", url); xhr.send()
    }

    // Recalculate the route from the current position when we have strayed off
    // the planned line (the driver took a different road). Rate-limited so GPS
    // jitter or a brief detour does not spam the routing service.
    function _reroute() {
        var p = gps.position
        if (!p.latitudeValid || !p.longitudeValid || !_destCoord) return
        _rerouting = true
        _offCount = 0
        _lastReroute = Date.now()
        summary = "Rerouting…"
        if (typeof navBridge !== "undefined" && navBridge)
            navBridge.setRerouting(true) // cluster: "rerouting" until the new route arrives
        mlog("off route by >" + offRouteM + "m — rerouting from "
             + p.coordinate.latitude.toFixed(4) + "," + p.coordinate.longitude.toFixed(4)
             + " heading " + (p.directionValid ? Math.round(p.direction) : "n/a"))
        _requestRoute(p.coordinate, _destCoord, _headingNow())
    }

    // Shortest distance (metres) from a point to the route polyline. The OSRM
    // geometry is dense (overview=full), so nearest-vertex is a good proxy.
    function _distanceToRoute(here) {
        var best = -1, c = _routeCoords
        for (var i = 0; i < c.length; i++) {
            var d = here.distanceTo(QtPositioning.coordinate(c[i][1], c[i][0]))
            if (best < 0 || d < best) best = d
        }
        return best
    }

    function _applyRoute(data) {
        if (!data.routes || data.routes.length === 0) { summary = "No route"; return }
        var r = data.routes[0]
        _routeCoords = (r.geometry && r.geometry.coordinates) ? r.geometry.coordinates : []
        var steps = []
        if (r.legs)
            for (var l = 0; l < r.legs.length; l++) {
                var sl = r.legs[l].steps || []
                for (var i = 0; i < sl.length; i++) {
                    var st = sl[i]
                    if (!st.maneuver || !st.maneuver.location) continue
                    steps.push({ text: _stepText(st), dir: _dirOf(st.maneuver),
                                 lon: st.maneuver.location[0], lat: st.maneuver.location[1], dist: st.distance,
                                 // for the instrument cluster (NavBridge.setUpcoming)
                                 navType: _navTypeOf(st), exit: st.maneuver.exit || 0,
                                 road: st.name || "", lanes: _lanesOf(st) })
                }
            }
        _steps = steps; _curStep = 0; _offCount = 0
        _totalM = Math.round(r.distance); _totalS = Math.round(r.duration)
        remainingM = _totalM; remainingS = _totalS
        etaText = Qt.formatTime(new Date(Date.now() + r.duration * 1000), "hh:mm")
        summary = _fmtDist(r.distance) + " · " + _fmtTime(r.duration)
        navigating = true; follow = true; zoom = drivingZoom
        _updateStep()
        mlog("route ready: " + _routeCoords.length + " pts, " + steps.length + " steps")
    }

    function clearRoute() {
        navigating = false
        _routeCoords = []; _steps = []; _curStep = 0; _destCoord = null
        _totalM = 0; _totalS = 0
        summary = ""; nextInstruction = ""; nextDistance = ""; destinationLabel = ""
        maneuverDir = 0; remainingM = 0; remainingS = 0; etaText = ""
        zoom = drivingZoom
        if (typeof navBridge !== "undefined" && navBridge) {
            navBridge.setUpcoming([])
            navBridge.update(false, 0, "", "", "", 0, 0, 0, "")
        }
    }

    // Desk self-test (preview only): exercise geocoding + routing with no user
    // input so the preview PNG/log show whether the HTTP nav chain works.
    function _selfTest() {
        mlog("nav self-test: geocode + route from " + centerLat.toFixed(3) + "," + centerLon.toFixed(3))
        search("Helsinki central railway station")
        navigateTo(QtPositioning.coordinate(centerLat + 0.012, centerLon + 0.02), "Test destination")
    }

    // Forward-only step tracker + instrument-cluster feed.
    function _updateStep() {
        if (_steps.length === 0) return
        var p = gps.position
        var here = (p.latitudeValid && p.longitudeValid) ? p.coordinate : null
        while (_curStep < _steps.length - 1 && here
               && here.distanceTo(QtPositioning.coordinate(_steps[_curStep].lat, _steps[_curStep].lon)) < 25)
            _curStep++
        var s = _steps[_curStep]
        maneuverDir = s.dir
        nextInstruction = s.text
        var dm = here ? Math.round(here.distanceTo(QtPositioning.coordinate(s.lat, s.lon))) : s.dist
        nextDistance = _fmtDist(dm)
        // Live route-remaining = distance to the next maneuver + the length of
        // every step after it; remaining time scaled by that distance fraction.
        // Both shrink monotonically as we drive, so the arrival ETA stays sane.
        var rem = dm
        for (var i = _curStep; i < _steps.length; i++) rem += _steps[i].dist
        remainingM = Math.round(rem)
        remainingS = (_totalM > 0) ? Math.round(_totalS * remainingM / _totalM) : _totalS
        etaText = Qt.formatTime(new Date(Date.now() + remainingS * 1000), "hh:mm")
        summary = _fmtDist(remainingM) + " · " + _fmtTime(remainingS)
        if (typeof navBridge !== "undefined" && navBridge) {
            // The next (up to) 3 maneuvers for the cluster; the first is the one
            // nextInstruction/maneuverDir describe.
            var up = []
            for (var k = _curStep; k < _steps.length && up.length < 3; k++) {
                var sk = _steps[k]
                up.push({ type: sk.navType, exit: sk.exit, road: sk.road, text: sk.text, lanes: sk.lanes })
            }
            navBridge.setUpcoming(up)
            // Current road = the road the previous maneuver put us on.
            var curRoad = _curStep > 0 ? _steps[_curStep - 1].road : ""
            navBridge.update(navigating, maneuverDir, nextInstruction, curRoad,
                             destinationLabel, dm, remainingM, remainingS, etaText)
        }

        // Arrival: once we're on the final step and within the arrival radius of
        // the destination, end navigation automatically (no manual stop needed).
        if (here && _destCoord && _curStep >= _steps.length - 1
                && here.distanceTo(_destCoord) < arrivalM) {
            mlog("arrived at destination; ending navigation")
            clearRoute()
            return
        }

        // Off-route detection: if we are consistently far from the planned line,
        // recalculate from here. A few consecutive samples (GPS updates ~1 Hz)
        // plus a rate limit avoid re-routing on noise or a single lane change.
        if (here && _routeCoords.length > 1 && !_rerouting) {
            if (_distanceToRoute(here) > offRouteM)
                _offCount++
            else
                _offCount = 0
            if (_offCount >= 3 && (Date.now() - _lastReroute) > 8000)
                _reroute()
        }
    }

    // OSRM maneuver (type + modifier) -> QtLocation RouteManeuver.Direction ints
    // (2-5 right family, 8-11 left, 6/7 u-turn, 0/1 straight) used by the arrow.
    function _dirOf(m) {
        var mod = m.modifier || ""
        if (m.type === "roundabout" || m.type === "rotary") return mod.indexOf("left") >= 0 ? 9 : 4
        switch (mod) {
        case "left": return 9
        case "slight left": return 10
        case "sharp left": return 8
        case "right": return 4
        case "slight right": return 3
        case "sharp right": return 5
        case "uturn": return 6
        default: return 1
        }
    }
    // OSRM step -> Android Auto NavigationType (== Car App Library
    // Maneuver.TYPE_*), so the cluster gets the real maneuver: roundabouts with
    // their exit number, forks, ramps, merges, arrival side. U-turns and
    // roundabout direction follow the step's driving side.
    function _navTypeOf(st) {
        var m = st.maneuver || {}, mod = m.modifier || "", t = m.type || ""
        var rightHand = (st.driving_side || "right") !== "left"
        if (st.mode === "ferry") return 37                          // FERRY_BOAT
        function turn(base) {                                     // TURN_* family
            switch (mod) {
            case "slight left": return 5
            case "slight right": return 6
            case "left": return 7
            case "right": return 8
            case "sharp left": return 9
            case "sharp right": return 10
            case "uturn": return rightHand ? 11 : 12
            default: return base
            }
        }
        function ramp(off) {
            var left = mod.indexOf("left") >= 0
            if (off) return mod.indexOf("slight") >= 0 ? (left ? 21 : 22) : (left ? 23 : 24)
            if (mod.indexOf("slight") >= 0) return left ? 13 : 14
            if (mod.indexOf("sharp") >= 0) return left ? 17 : 18
            if (mod === "uturn") return rightHand ? 19 : 20
            return left ? 15 : (mod.indexOf("right") >= 0 ? 16 : 36)
        }
        switch (t) {
        case "depart": return 1                                      // DEPART
        case "arrive":
            return mod.indexOf("left") >= 0 ? 41 : (mod.indexOf("right") >= 0 ? 42 : 39)
        case "new name": return 2                                    // NAME_CHANGE
        case "fork":
            return mod.indexOf("left") >= 0 ? 25 : (mod.indexOf("right") >= 0 ? 26 : 36)
        case "merge":
            return mod.indexOf("left") >= 0 ? 27 : (mod.indexOf("right") >= 0 ? 28 : 29)
        case "on ramp": return ramp(false)
        case "off ramp": return ramp(true)
        case "roundabout": case "rotary":
            // ROUNDABOUT_ENTER_AND_EXIT_CCW (34) for right-hand traffic, _CW (32) otherwise
            return rightHand ? 34 : 32
        case "exit roundabout": case "exit rotary": return 31        // ROUNDABOUT_EXIT
        default: return turn(36)                                      // STRAIGHT
        }
    }
    // Lane guidance at a step's maneuver: OSRM puts the approach lanes on the
    // step's first intersection. Each lane -> [{shape, hl}], shape per
    // NavigationLaneDirection; hl marks directions usable for this maneuver.
    function _lanesOf(st) {
        var ints = st.intersections
        if (!ints || !ints.length || !ints[0].lanes) return []
        var mod = (st.maneuver && st.maneuver.modifier) || "straight"
        var rightHand = (st.driving_side || "right") !== "left"
        var shapes = { "straight": 1, "slight left": 2, "slight right": 3, "left": 4,
                       "right": 5, "sharp left": 6, "sharp right": 7,
                       "uturn": rightHand ? 8 : 9, "none": 0 }
        var out = []
        for (var i = 0; i < ints[0].lanes.length; i++) {
            var ln = ints[0].lanes[i], ind = ln.indications || ["none"]
            var match = ind.indexOf(mod) >= 0
            var dirs = []
            for (var j = 0; j < ind.length; j++) {
                var s = shapes[ind[j]]
                dirs.push({ shape: s === undefined ? 0 : s,
                            hl: !!ln.valid && (!match || ind[j] === mod) })
            }
            out.push(dirs)
        }
        return out
    }
    function _stepText(st) {
        var m = st.maneuver, mod = m.modifier || "", name = st.name || ""
        var onName = name ? " onto " + name : ""
        switch (m.type) {
        case "depart": return "Head out" + (name ? " on " + name : "")
        case "arrive": return "Arrive at destination"
        case "turn": case "end of road":
            return mod === "straight" ? "Continue" + onName : "Turn " + mod + onName
        case "exit roundabout": case "exit rotary": return "Exit the roundabout" + onName
        case "new name": return "Continue" + (name ? " on " + name : "")
        case "continue": return "Continue" + (mod ? " " + mod : "") + onName
        case "merge": return "Merge" + onName
        case "on ramp": return "Take the ramp" + onName
        case "off ramp": return "Take the exit" + onName
        case "fork": return "Keep " + (mod || "straight") + onName
        case "roundabout": case "rotary":
            var ex = m.exit || 0
            var nth = ex === 1 ? "1st" : ex === 2 ? "2nd" : ex === 3 ? "3rd" : ex + "th"
            return "At the roundabout, " + (ex > 0 ? "take the " + nth + " exit" : "exit") + onName
        default: return (mod ? "Turn " + mod : "Continue") + onName
        }
    }
    function _fmtDist(m) {
        if (m >= 1000) return (m / 1000).toFixed(m < 10000 ? 1 : 0) + " km"
        return Math.round(m / 10) * 10 + " m"
    }
    function _fmtTime(s) {
        var min = Math.round(s / 60)
        if (min >= 60) return Math.floor(min / 60) + " h " + (min % 60) + " min"
        return min + " min"
    }

    // --- Web-Mercator helpers (lon/lat <-> fractional tile coordinates) ---
    function lon2tx(lon, z) { return (lon + 180.0) / 360.0 * Math.pow(2, z) }
    function lat2ty(lat, z) {
        var r = lat * Math.PI / 180.0
        return (1.0 - Math.log(Math.tan(r) + 1.0 / Math.cos(r)) / Math.PI) / 2.0 * Math.pow(2, z)
    }

    // World-pixel coords. Everything in the tile layer is projected against the
    // snapped anchor (_rLat/_rLon); the layer is then translated by the offset to
    // the live display centre (_panX/_panY) so panning needs no tile rebuild.
    readonly property real _n: Math.pow(2, zoom)
    readonly property real _rcxWorld: lon2tx(_rLon, zoom) * tileSize
    readonly property real _rcyWorld: lat2ty(_rLat, zoom) * tileSize
    readonly property real _originX: _rcxWorld - _canvas / 2
    readonly property real _originY: _rcyWorld - _canvas / 2
    readonly property real _panX: _rcxWorld - lon2tx(centerLon, zoom) * tileSize
    readonly property real _panY: _rcyWorld - lat2ty(centerLat, zoom) * tileSize

    // Screen pixel (in the tile-layer frame) for a coordinate.
    function screenX(lon) { return lon2tx(lon, zoom) * tileSize - _originX }
    function screenY(lat) { return lat2ty(lat, zoom) * tileSize - _originY }

    // Re-anchor the tiles to the current car position once it has drifted ~a tile
    // from the anchor (so the sub-tile translate stays within the padded grid).
    function _resnapIfNeeded(c) {
        var ts = tileSize
        var dx = (lon2tx(c.longitude, zoom) - lon2tx(_rLon, zoom)) * ts
        var dy = (lat2ty(c.latitude, zoom) - lat2ty(_rLat, zoom)) * ts
        if (Math.abs(dx) > ts || Math.abs(dy) > ts || _tiles.length === 0) {
            _rLat = c.latitude
            _rLon = c.longitude
        }
    }

    // Tiles covering the canvas + a one-tile pad each side (so a sub-anchor
    // translate of up to ~a tile still has tiles under the whole visible area).
    function buildTiles(lat, lon, z, pad) {
        if (width <= 0 || height <= 0) return []
        var ts = tileSize, n = Math.pow(2, z)
        var ox = lon2tx(lon, z) * ts - _canvas / 2
        var oy = lat2ty(lat, z) * ts - _canvas / 2
        var xa = Math.floor((ox - pad) / ts), xb = Math.floor((ox + _canvas + pad) / ts)
        var ya = Math.floor((oy - pad) / ts), yb = Math.floor((oy + _canvas + pad) / ts)
        var out = []
        for (var tx = xa; tx <= xb; tx++) {
            for (var ty = ya; ty <= yb; ty++) {
                if (ty < 0 || ty >= n) continue          // no wrap in Y
                var wx = ((tx % n) + n) % n               // wrap X around the globe
                out.push({ sx: tx * ts - ox, sy: ty * ts - oy, z: z, x: wx, y: ty,
                           url: tileHost + z + "/" + wx + "/" + ty + ".png" })
            }
        }
        return out
    }
    readonly property var _tiles: buildTiles(_rLat, _rLon, zoom, tileSize * 2)

    Rectangle { anchors.fill: parent; color: tm.night ? "#0b0f14" : "#aadaff" } // sea/void backdrop

    // Everything map-space (tiles, traffic, route, pins, car) lives here and
    // rotates as one unit: north-up = no rotation; heading-up = -heading so the
    // travel direction points up. Centred on the car (= the viewport centre).
    Item {
        id: mapContent
        width: tm._canvas; height: tm._canvas
        x: (tm.width - width) / 2
        y: (tm.height - height) / 2
        transformOrigin: Item.Center
        rotation: tm.headingUp ? -tm._dispHeading : 0
        Behavior on rotation { RotationAnimation { duration: 220; direction: RotationAnimation.Shortest } }

    // Tiles + route + pins are built against the snapped anchor and translated by
    // the sub-anchor offset so the map glides between 1 Hz GPS fixes.
    Item {
        id: tileLayer
        width: tm._canvas; height: tm._canvas
        x: tm._panX; y: tm._panY

    Repeater {
        model: tm._tiles
        delegate: Image {
            x: modelData.sx; y: modelData.sy
            width: tm.tileSize; height: tm.tileSize
            asynchronous: true
            cache: true
            source: modelData.url
            // Appear instantly once ready (no fade): on each re-anchor the
            // delegates are rebuilt, and a fade would flash the whole map.
            opacity: status === Image.Ready ? 1 : 0
            onStatusChanged: {
                if (status === Image.Error && !tm._tileErrLogged) {
                    tm._tileErrLogged = true
                    tm.mlog("TileMap tile ERROR: " + source)
                } else if (status === Image.Ready && !tm._tileErrLogged && modelData.sx === 0) {
                    // log once that tiles are arriving
                    tm.mlog("TileMap tiles loading OK")
                }
            }
        }
    }

    // Night tint over the (light) OSM tiles.
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: tm.night ? 0.32 : 0.0
        Behavior on opacity { NumberAnimation { duration: 250 } }
    }

    // Live traffic flow tiles (coloured roads: green=clear → red=jam), laid over
    // the base map and the night tint so the colours stay vivid. Same tile grid
    // as the base layer, so it covers exactly the on-screen area that follows the
    // car. Only requested when a TomTom key is set and the overlay is enabled.
    Repeater {
        model: (tm.trafficOn && tm.trafficKey) ? tm._tiles : []
        delegate: Image {
            x: modelData.sx; y: modelData.sy
            width: tm.tileSize; height: tm.tileSize
            asynchronous: true; cache: true
            source: tm.trafficUrl(modelData.z, modelData.x, modelData.y)
            opacity: status === Image.Ready ? 1 : 0
            onStatusChanged: if (status === Image.Error && !tm._trafficErrLogged) {
                tm._trafficErrLogged = true
                tm.mlog("traffic tile ERROR (check the TomTom key): " + source)
            }
        }
    }

    // Route polyline (white casing + blue line), projected through the same
    // web-mercator transform as the tiles. Repaints as the map pans/zooms
    // (_originX/_originY change) or when a new route arrives.
    Canvas {
        id: routeCanvas
        anchors.fill: parent
        visible: tm.navigating && tm._routeCoords.length > 1
        property real k: tm._originX + tm._originY + tm._routeCoords.length
        onKChanged: requestPaint()
        onVisibleChanged: requestPaint()
        function _trace(ctx) {
            ctx.beginPath()
            var c = tm._routeCoords
            for (var i = 0; i < c.length; i++) {
                var x = tm.screenX(c[i][0]), y = tm.screenY(c[i][1])
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
        }
        onPaint: {
            var ctx = getContext("2d"); ctx.reset()
            if (!tm.navigating || tm._routeCoords.length < 2) return
            ctx.lineJoin = "round"; ctx.lineCap = "round"
            ctx.strokeStyle = "white"; ctx.lineWidth = Math.max(8, tm.width * 0.016); _trace(ctx); ctx.stroke()
            ctx.strokeStyle = "#4c8dff"; ctx.lineWidth = Math.max(5, tm.width * 0.011); _trace(ctx); ctx.stroke()
        }
    }

    // Destination pin.
    Rectangle {
        visible: tm.navigating && tm._destCoord !== null
        width: Math.max(18, tm.width * 0.03); height: width; radius: width / 2
        x: (tm._destCoord ? tm.screenX(tm._destCoord.longitude) : 0) - width / 2
        y: (tm._destCoord ? tm.screenY(tm._destCoord.latitude) : 0) - height
        color: "#ff3b30"; border.color: "white"; border.width: Math.max(2, width * 0.14)
    }
    } // tileLayer (translated for smooth panning)

    // Current-position marker: a heading arrow while moving (GPS course over
    // ground), a plain dot when stationary (no valid heading). Always at the
    // canvas centre (= viewport centre) since the map pans under the car.
    Item {
        id: marker
        visible: gps.position.latitudeValid && gps.position.longitudeValid
        width: Math.max(26, tm.width * 0.05); height: width
        x: tm._canvas / 2 - width / 2
        y: tm._canvas / 2 - height / 2
        property bool hasHeading: gps.position.directionValid

        // Rotating arrow (0° = north, clockwise), shown only with a heading.
        Item {
            anchors.fill: parent
            visible: marker.hasHeading
            rotation: gps.position.direction
            Canvas {
                id: arrow
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d"); ctx.reset();
                    var w = width, h = height, cx = w / 2;
                    ctx.beginPath();
                    ctx.moveTo(cx, h * 0.06);          // tip (points "up" = heading)
                    ctx.lineTo(w * 0.82, h * 0.92);    // right wing
                    ctx.lineTo(cx, h * 0.68);          // notch
                    ctx.lineTo(w * 0.18, h * 0.92);    // left wing
                    ctx.closePath();
                    ctx.fillStyle = "#4c8dff";
                    ctx.fill();
                    ctx.lineWidth = Math.max(2, w * 0.07);
                    ctx.lineJoin = "round";
                    ctx.strokeStyle = "white";
                    ctx.stroke();
                }
            }
        }
        // Plain dot when GPS reports no heading (parked / standing still).
        Rectangle {
            anchors.centerIn: parent
            visible: !marker.hasHeading
            width: parent.width * 0.6; height: width; radius: width / 2
            color: "#4c8dff"; border.color: "white"; border.width: Math.max(2, width * 0.14)
        }
    }
    } // mapContent (rotates with the map)

    // "© OpenStreetMap" attribution (required by the tile usage policy).
    Rectangle {
        anchors.right: parent.right; anchors.bottom: parent.bottom
        width: credit.width + 10; height: credit.height + 4
        color: tm.night ? "#99000000" : "#99ffffff"
        Text {
            id: credit; anchors.centerIn: parent
            text: "© OpenStreetMap"
            color: tm.night ? "#cfe0f0" : "#334"
            font.pixelSize: Math.max(10, tm.height * 0.022)
        }
    }

    PositionSource {
        id: gps
        active: true
        updateInterval: 1000
        onSourceErrorChanged: tm.mlog("GPS sourceError=" + sourceError + " (0=none,2=closed,...)")
        onActiveChanged: tm.mlog("GPS active=" + active + " valid=" + valid)
        onPositionChanged: {
            if (position.latitudeValid && position.longitudeValid) {
                if (!tm._fixLogged) {
                    tm._fixLogged = true
                    tm.mlog("GPS fix " + position.coordinate.latitude.toFixed(4) + "," + position.coordinate.longitude.toFixed(4))
                    // Now that we know where we are, zoom from the wide "waiting"
                    // view to a street-level driving zoom (unless a route already
                    // set its own zoom).
                    if (!tm.navigating && tm.zoom < tm.drivingZoom)
                        tm.zoom = tm.drivingZoom
                }
                if (tm.follow) {
                    // First fix: snap instantly (don't animate a fly from Helsinki);
                    // afterwards the Behavior glides the display centre between fixes.
                    tm.centerLat = position.coordinate.latitude
                    tm.centerLon = position.coordinate.longitude
                    tm._resnapIfNeeded(position.coordinate)
                    tm._animatePan = true
                }
                // Drive heading-up rotation; keep the last heading when stopped
                // (directionValid goes false at a standstill) so the map doesn't snap.
                if (position.directionValid)
                    tm._dispHeading = position.direction
                // Current speed (m/s -> km/h). Clamp tiny GPS jitter to 0 so a
                // parked car reads 0, not 1-2 km/h of noise.
                if (position.speedValid && position.speed >= 0)
                    tm.speedKmh = position.speed < 0.8 ? 0 : Math.round(position.speed * 3.6)
                if (tm.navigating) tm._updateStep()
                tm._checkSpeedLimit(position.coordinate)
            }
        }
    }
}
