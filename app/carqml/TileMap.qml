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
    property bool _fixLogged: false
    Component.onCompleted: mlog("TileMap loaded; host=" + tileHost + " tiles=" + _tiles.length)

    // Centre of the view. Defaults to Helsinki until GPS gets a fix.
    property real centerLat: 60.1699
    property real centerLon: 24.9384
    property int zoom: 15
    readonly property int tileSize: 256
    property bool night: (typeof carScreen !== "undefined" && carScreen) ? carScreen.nightMode : false
    property string tileHost: (typeof osmTileHost !== "undefined" && osmTileHost)
                              ? osmTileHost : "https://tile.openstreetmap.org/"
    property bool follow: true   // keep the view centred on GPS

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
    // route internals
    property var _routeCoords: []            // [[lon,lat], ...] drawn polyline
    property var _steps: []                   // [{text,dir,lat,lon,dist}]
    property int _curStep: 0
    property var _destCoord: null

    // --- geocoding (Nominatim) + routing (OSRM) over plain HTTP. QtLocation's
    // osm plugin fails to load on-device, so we talk to the public OSM services
    // directly; the engine's NAM factory stamps the required User-Agent. ---
    function search(text) {
        if (!text || text.length < 2) { geocodeModel = []; return }
        searching = true
        var url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=8&q=" + encodeURIComponent(text)
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            searching = false
            if (xhr.status !== 200) { mlog("geocode http " + xhr.status); geocodeModel = []; return }
            try {
                var arr = JSON.parse(xhr.responseText), out = []
                for (var i = 0; i < arr.length; i++)
                    out.push({ address: { text: arr[i].display_name },
                               coordinate: QtPositioning.coordinate(parseFloat(arr[i].lat), parseFloat(arr[i].lon)) })
                geocodeModel = out
                mlog("geocode '" + text + "' -> " + out.length)
            } catch (e) { mlog("geocode parse err " + e); geocodeModel = [] }
        }
        xhr.open("GET", url); xhr.send()
    }

    function navigateTo(coord, label) {
        if (!coord || !coord.isValid) return
        var from = (gps.position.latitudeValid && gps.position.longitudeValid)
                   ? gps.position.coordinate : QtPositioning.coordinate(centerLat, centerLon)
        destinationLabel = label || ""
        _destCoord = coord
        summary = "Routing…"
        var url = "https://router.project-osrm.org/route/v1/driving/"
                + from.longitude + "," + from.latitude + ";" + coord.longitude + "," + coord.latitude
                + "?overview=full&geometries=geojson&steps=true"
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE) return
            if (xhr.status !== 200) { summary = "No route"; mlog("route http " + xhr.status); return }
            try { _applyRoute(JSON.parse(xhr.responseText)) }
            catch (e) { summary = "No route"; mlog("route parse err " + e) }
        }
        xhr.open("GET", url); xhr.send()
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
                                 lon: st.maneuver.location[0], lat: st.maneuver.location[1], dist: st.distance })
                }
            }
        _steps = steps; _curStep = 0
        remainingM = Math.round(r.distance); remainingS = Math.round(r.duration)
        etaText = Qt.formatTime(new Date(Date.now() + r.duration * 1000), "hh:mm")
        summary = _fmtDist(r.distance) + " · " + _fmtTime(r.duration)
        navigating = true; follow = true; zoom = 16
        _updateStep()
        mlog("route ready: " + _routeCoords.length + " pts, " + steps.length + " steps")
    }

    function clearRoute() {
        navigating = false
        _routeCoords = []; _steps = []; _curStep = 0; _destCoord = null
        summary = ""; nextInstruction = ""; nextDistance = ""; destinationLabel = ""
        maneuverDir = 0; remainingM = 0; remainingS = 0; etaText = ""
        zoom = 15
        if (typeof navBridge !== "undefined" && navBridge)
            navBridge.update(false, 0, "", "", "", 0, 0, 0, "")
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
        if (here && _destCoord) remainingM = Math.round(here.distanceTo(_destCoord))
        if (typeof navBridge !== "undefined" && navBridge)
            navBridge.update(navigating, maneuverDir, nextInstruction, "",
                             destinationLabel, dm, remainingM, remainingS, etaText)
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
    function _stepText(st) {
        var m = st.maneuver, mod = m.modifier || "", name = st.name || ""
        var onName = name ? " onto " + name : ""
        switch (m.type) {
        case "depart": return "Head out" + (name ? " on " + name : "")
        case "arrive": return "Arrive at destination"
        case "turn": case "end of road": return "Turn " + mod + onName
        case "new name": return "Continue" + (name ? " on " + name : "")
        case "continue": return "Continue" + (mod ? " " + mod : "") + onName
        case "merge": return "Merge" + onName
        case "on ramp": return "Take the ramp" + onName
        case "off ramp": return "Take the exit" + onName
        case "fork": return "Keep " + (mod || "straight") + onName
        case "roundabout": case "rotary": return "At the roundabout, exit" + onName
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

    // World-pixel coordinate of the view centre, and of the top-left corner.
    readonly property real _n: Math.pow(2, zoom)
    readonly property real _cxWorld: lon2tx(centerLon, zoom) * tileSize
    readonly property real _cyWorld: lat2ty(centerLat, zoom) * tileSize
    readonly property real _originX: _cxWorld - width / 2
    readonly property real _originY: _cyWorld - height / 2

    // Screen pixel for an arbitrary coordinate (used for markers/overlays).
    function screenX(lon) { return lon2tx(lon, zoom) * tileSize - _originX }
    function screenY(lat) { return lat2ty(lat, zoom) * tileSize - _originY }

    // Build the list of visible tiles: {sx, sy, url}. Re-evaluated whenever the
    // centre, zoom or size change.
    function buildTiles(lat, lon, z, w, h) {
        if (w <= 0 || h <= 0) return []
        var ts = tileSize
        var n = Math.pow(2, z)
        var cx = lon2tx(lon, z) * ts
        var cy = lat2ty(lat, z) * ts
        var ox = cx - w / 2, oy = cy - h / 2
        var xa = Math.floor(ox / ts), xb = Math.floor((ox + w) / ts)
        var ya = Math.floor(oy / ts), yb = Math.floor((oy + h) / ts)
        var out = []
        for (var tx = xa; tx <= xb; tx++) {
            for (var ty = ya; ty <= yb; ty++) {
                if (ty < 0 || ty >= n) continue          // no wrap in Y
                var wx = ((tx % n) + n) % n               // wrap X around the globe
                out.push({ sx: tx * ts - ox, sy: ty * ts - oy,
                           url: tileHost + z + "/" + wx + "/" + ty + ".png" })
            }
        }
        return out
    }
    readonly property var _tiles: buildTiles(centerLat, centerLon, zoom, width, height)

    Rectangle { anchors.fill: parent; color: tm.night ? "#0b0f14" : "#aadaff" } // sea/void backdrop

    Repeater {
        model: tm._tiles
        delegate: Image {
            x: modelData.sx; y: modelData.sy
            width: tm.tileSize; height: tm.tileSize
            asynchronous: true
            cache: true
            source: modelData.url
            // Fade a tile in once it loads; blank stays on the backdrop.
            opacity: status === Image.Ready ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 180 } }
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

    // Current-position marker: a heading arrow while moving (GPS course over
    // ground), a plain dot when stationary (no valid heading).
    Item {
        id: marker
        visible: gps.position.latitudeValid && gps.position.longitudeValid
        width: Math.max(26, tm.width * 0.05); height: width
        x: tm.screenX(tm.centerLon) - width / 2
        y: tm.screenY(tm.centerLat) - height / 2
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
                }
                if (tm.follow) {
                    tm.centerLat = position.coordinate.latitude
                    tm.centerLon = position.coordinate.longitude
                }
                if (tm.navigating) tm._updateStep()
            }
        }
    }
}
