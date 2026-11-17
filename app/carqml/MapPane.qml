import QtQuick 2.0
import QtPositioning 5.0
import QtLocation 5.0

// Real OSM map for the car UI: live GPS, destination search (geocoding),
// route calculation and drawing, and a "next instruction" for the turn card.
// Loaded via a Loader so CarMain can fall back to its placeholder if
// QtLocation/the osm plugin are missing. Theming follows carScreen.nightMode.
Item {
    id: mp
    function mlog(s) { if (typeof carScreen !== "undefined" && carScreen) carScreen.log("map: " + s); else console.log("map: " + s) }
    Component.onCompleted: mlog("MapPane loaded; providers=" + osmPlugin.availableServiceProviders
                                + " types@init=" + map.supportedMapTypes.length)
    readonly property bool night: (typeof carScreen !== "undefined" && carScreen) ? carScreen.nightMode : false
    // False when the geoservices plugin can't provide tiles (missing osm plugin,
    // or QtLocation 5.4's dead provider repository): CarMain then shows its
    // placeholder instead of a blank pane.
    readonly property bool usable: map.supportedMapTypes.length > 0

    // --- state the search UI (CarMain) reads/drives ---
    property alias geocodeModel: geocode     // search results model
    property bool searching: false           // a geocode request is in flight
    property bool navigating: false          // a route is active
    property string destinationLabel: ""
    property string summary: ""              // "12 km · 18 min"
    property string nextInstruction: ""      // e.g. "Turn right onto Main St"
    property string nextDistance: ""         // e.g. "300 m"
    // turn-by-turn detail (also fed to the car instrument cluster, Phase 3)
    property int maneuverDir: 0              // QtLocation RouteManeuver.Direction of the next step
    property int stepDistanceM: 0            // metres to the next maneuver
    property int stepTimeS: 0               // seconds to the next maneuver
    property int remainingM: 0               // metres to destination
    property int remainingS: 0               // seconds to destination
    property string etaText: ""              // arrival clock, e.g. "15:42"
    property var steps: []                    // [{text,dir,pos}] maneuvers along the route
    property int currentStep: 0              // forward-only index into steps
    signal navUpdated()                       // emitted whenever the above change (cluster bridge)

    // Push turn-by-turn to the car's instrument cluster via the C++ NavBridge
    // (context property "navBridge"); guarded so the map still works without it.
    onNavUpdated: {
        if (typeof navBridge === "undefined" || !navBridge) return
        navBridge.update(navigating, maneuverDir, nextInstruction, "",
                         destinationLabel, stepDistanceM, remainingM, remainingS, etaText)
    }

    // Tile server (keyless OSM by default; osmTileHost overrides for offline).
    readonly property string tileHost: (typeof osmTileHost !== "undefined" && osmTileHost)
                                       ? osmTileHost : "https://tile.openstreetmap.org/"
    Plugin {
        id: osmPlugin
        name: "osm"
        PluginParameter { name: "osm.mapping.providersrepository.disabled"; value: true }
        PluginParameter { name: "osm.mapping.host"; value: mp.tileHost }
        PluginParameter { name: "osm.mapping.copyright"; value: "© OpenStreetMap contributors" }
        PluginParameter { name: "osm.mapping.highdpi_tiles"; value: true }
        // With the providers repository disabled we must name the geocoding and
        // routing endpoints ourselves (public OSM servers; fine for personal use).
        PluginParameter { name: "osm.geocoding.host"; value: "https://nominatim.openstreetmap.org/" }
        PluginParameter { name: "osm.routing.host"; value: "https://router.project-osrm.org/route/v1/driving/" }
        PluginParameter { name: "osm.useragent"; value: "Carfish/0.1 (personal navigation)" }
    }

    // --- geocoding (destination search) ---
    GeocodeModel {
        id: geocode
        plugin: osmPlugin
        autoUpdate: false
        limit: 12
        onStatusChanged: if (status === GeocodeModel.Ready || status === GeocodeModel.Error)
                             mp.searching = false
    }

    // --- routing ---
    RouteQuery { id: routeQuery }
    RouteModel {
        id: routeModel
        plugin: osmPlugin
        query: routeQuery
        autoUpdate: false
        onStatusChanged: {
            if (status === RouteModel.Ready && count > 0) {
                var r = get(0)
                // collect the valid maneuvers (skip the "depart" and any invalid)
                var s = []
                for (var i = 0; i < r.segments.length; i++) {
                    var m = r.segments[i].maneuver
                    if (m && m.valid && m.instructionText.length > 0)
                        s.push({ text: m.instructionText, dir: m.direction, pos: m.position })
                }
                mp.steps = s
                mp.currentStep = 0
                mp.remainingM = r.distance
                mp.remainingS = r.travelTime
                mp.etaText = Qt.formatTime(new Date(Date.now() + r.travelTime * 1000), "hh:mm")
                mp.summary = mp.fmtDistance(r.distance) + " · " + mp.fmtTime(r.travelTime)
                mp.navigating = true
                map.zoomLevel = 17
                mp.updateNextInstruction()
            } else if (status === RouteModel.Error) {
                mp.summary = "No route"
            }
        }
    }

    Map {
        id: map
        anchors.fill: parent
        plugin: osmPlugin
        zoomLevel: 15
        gesture.enabled: false // car UI: auto-follow GPS
        onSupportedMapTypesChanged: mp.mlog("MapPane types=" + supportedMapTypes.length
                                            + (supportedMapTypes.length > 0 ? " [" + supportedMapTypes[0].name + "]" : ""))
        onErrorChanged: mp.mlog("MapPane error=" + error + " " + errorString)
        activeMapType: supportedMapTypes.length > 0 ? supportedMapTypes[supportedMapTypes.length - 1]
                                                     : activeMapType
        center: gps.position.coordinate.isValid ? gps.position.coordinate
                                                : QtPositioning.coordinate(60.1699, 24.9384)

        // route polyline
        MapItemView {
            model: routeModel
            delegate: MapRoute {
                route: routeData
                line.color: "#4c8dff"
                line.width: Math.max(6, mp.width * 0.012)
                opacity: 0.85
            }
        }
        // destination marker
        MapQuickItem {
            visible: mp.navigating && routeQuery.waypoints.length > 1
            coordinate: routeQuery.waypoints.length > 1 ? routeQuery.waypoints[routeQuery.waypoints.length - 1]
                                                         : QtPositioning.coordinate()
            anchorPoint.x: pin.width / 2; anchorPoint.y: pin.height
            sourceItem: Rectangle {
                id: pin
                width: Math.max(22, mp.width * 0.035); height: width; radius: width / 2
                color: "#ff3b30"; border.color: "white"; border.width: Math.max(2, width * 0.12)
            }
        }
        // current position
        MapQuickItem {
            id: here
            visible: gps.position.coordinate.isValid
            coordinate: gps.position.coordinate
            anchorPoint.x: dot.width / 2; anchorPoint.y: dot.height / 2
            sourceItem: Rectangle {
                id: dot
                width: Math.max(18, mp.width * 0.03); height: width; radius: width / 2
                color: "#4c8dff"; border.color: "white"; border.width: Math.max(2, width * 0.14)
            }
        }
    }

    PositionSource {
        id: gps
        active: true
        updateInterval: 1000
        onPositionChanged: if (mp.navigating) mp.updateNextInstruction()
    }


    // Subtle darkening at night.
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: mp.night ? 0.32 : 0.0
        Behavior on opacity { NumberAnimation { duration: 250 } }
    }


    // --- API used by the search UI ---
    function search(text) {
        if (text.length < 2) return
        searching = true
        geocode.query = text
        geocode.update()
    }

    function navigateTo(coord, label) {
        if (!coord || !coord.isValid) return
        var from = gps.position.coordinate.isValid ? gps.position.coordinate : map.center
        routeQuery.clearWaypoints()
        routeQuery.addWaypoint(from)
        routeQuery.addWaypoint(coord)
        routeQuery.travelModes = RouteQuery.CarTravel
        destinationLabel = label || ""
        summary = "Routing…"
        routeModel.update()
        map.center = coord
    }

    function clearRoute() {
        navigating = false
        routeModel.reset()
        routeQuery.clearWaypoints()
        steps = []; currentStep = 0
        summary = ""; nextInstruction = ""; nextDistance = ""; destinationLabel = ""
        maneuverDir = 0; stepDistanceM = 0; remainingM = 0; remainingS = 0; etaText = ""
        map.zoomLevel = 15
        if (gps.position.coordinate.isValid) map.center = gps.position.coordinate
        navUpdated()
    }

    // Forward-only step tracker: advance past a maneuver once we're within ~25 m
    // of it, and report the next one (text, direction, distance) for the turn
    // card and the instrument cluster.
    function updateNextInstruction() {
        if (steps.length === 0) return
        var pos = gps.position.coordinate
        while (currentStep < steps.length - 1 && pos.isValid
               && pos.distanceTo(steps[currentStep].pos) < 25)
            currentStep++
        var st = steps[currentStep]
        maneuverDir = st.dir
        nextInstruction = st.text
        stepDistanceM = pos.isValid ? Math.round(pos.distanceTo(st.pos)) : 0
        nextDistance = fmtDistance(stepDistanceM)
        if (pos.isValid && routeQuery.waypoints.length > 1)
            remainingM = Math.round(pos.distanceTo(routeQuery.waypoints[routeQuery.waypoints.length - 1]))
        if (pos.isValid) map.center = pos   // follow the car along the route
        navUpdated()
    }

    function fmtDistance(m) {
        if (m >= 1000) return (m / 1000).toFixed(m < 10000 ? 1 : 0) + " km"
        return Math.round(m / 10) * 10 + " m"
    }
    function fmtTime(s) {
        var min = Math.round(s / 60)
        if (min >= 60) return Math.floor(min / 60) + " h " + (min % 60) + " min"
        return min + " min"
    }
}
