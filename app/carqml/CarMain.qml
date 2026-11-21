import QtQuick 2.6
import QtPositioning 5.0

// The UI projected to the head unit. Plain QtQuick (no Silica): it renders
// offscreen at the car's resolution (CarScreen sizes this root to the
// negotiated content rect) and receives the car's touch events.
//
// Layout mirrors Android Auto's split screen: a navigation rail, a large map
// pane and a "now playing" pane, plus a status bar. It reflows between
// landscape and portrait from the aspect ratio alone, so the same scene works
// on a wide dashboard or a tall portrait head unit.
Rectangle {
    id: root

    // Preview-only: Controller sets this true to run a no-input nav self-test.
    property bool previewTest: false
    onPreviewTestChanged: if (previewTest && navMap && navMap._selfTest) navMap._selfTest()
    // Preview-only: open the full-screen map state / search overlay / a nav
    // self-test, so stills can show those states (host preview).
    Component.onCompleted: {
        if (typeof previewExpanded !== "undefined" && previewExpanded === "1")
            root.mapExpanded = true
        if (typeof previewSearch !== "undefined" && previewSearch === "1") {
            root.mapExpanded = true; root.searching = true
            root.searchText = (typeof previewSearchText !== "undefined" && previewSearchText)
                              ? previewSearchText : "Main Street 12"
            searchPreview.start()
        }
        if (typeof previewNav !== "undefined" && previewNav === "1") {
            root.mapExpanded = true; navPreview.start()
        }
        if (typeof previewPlaces !== "undefined" && previewPlaces === "1") {
            root.mapExpanded = true; root.searching = true
        }
        if (typeof previewChargers !== "undefined" && previewChargers === "1")
            chargersPreview.start()
        if (typeof previewQueue !== "undefined" && previewQueue === "1")
            root.showQueue = true
        if (typeof previewHeadingUp !== "undefined" && previewHeadingUp === "1")
            hdgPreview.start()
    }
    // Preview-only: force heading-up rotation (no GPS off-car) to check the map
    // rotates and the compass turns.
    Timer {
        id: hdgPreview; interval: 700
        onTriggered: if (root.navMap) { root.navMap._dispHeading = 40; carScreen.mapHeadingUp = true; root.navMap.speedKmh = 48 }
    }
    // Preview-only: open the charger list (live Overpass query) once the map loads.
    Timer {
        id: chargersPreview; interval: 700
        onTriggered: { root.mapExpanded = true; root.openChargers() }
    }
    // Preview-only: run the destination search once the map has loaded, so a
    // still shows live (distance-sorted) results.
    Timer {
        id: searchPreview; interval: 700
        onTriggered: if (root.navMap) {
            // PREVIEW_CENTER=lat,lon searches from there (no GPS on the desk).
            if (typeof previewCenter !== "undefined" && previewCenter) {
                var ll = previewCenter.split(",")
                root.navMap.centerLat = parseFloat(ll[0]); root.navMap.centerLon = parseFloat(ll[1])
            }
            root.navMap.search(root.searchText)
        }
    }
    // Preview-only: fill a sample navigation state once the map has loaded, so a
    // still shows the turn card + arrival bar without needing the network.
    Timer {
        id: navPreview; interval: 700
        onTriggered: if (root.navMap) {
            root.navMap.navigating = true
            root.navMap.destinationLabel = "Main Street 12"
            root.navMap.nextInstruction = "Turn right onto Mannerheimintie"
            root.navMap.nextDistance = "300 m"
            root.navMap.maneuverDir = 4
            root.navMap.summary = "12 km · 18 min"
            root.navMap.etaText = "14:32"
            root.navMap.speedKmh = 48
            root.navMap.remainingM = 12000
            // PREVIEW_LIMIT=<n> shows a posted speed limit (48 km/h > 40 = speeding).
            if (typeof previewLimit !== "undefined" && previewLimit) root.navMap.speedLimit = parseInt(previewLimit)
            // PREVIEW_EV_PCT=<n> sets the car's charge so stills can show the
            // amber/red arrival states and the low-battery warning.
            var soc = (typeof previewEvPct !== "undefined" && previewEvPct) ? parseInt(previewEvPct) : 72
            if (typeof carScreen !== "undefined" && carScreen && carScreen.setEnergy)
                carScreen.setEnergy(soc, Math.round(soc * 2.9), true, soc < 15) // ~290 km at 100%
        }
    }

    // Route a diagnostic line to the device's durable log (~/carfish.log)
    // via the C++ carScreen bridge; a no-op in the plain host preview.
    function mlog(s) {
        if (typeof carScreen !== "undefined" && carScreen)
            carScreen.log("map: " + s)
        else
            console.log("map: " + s)
    }

    // --- orientation & metrics (everything scales off the shorter edge) ---
    // NB: these bindings reference their siblings via the explicit `root.`
    // prefix. A bare reference (e.g. `pad`) to another of root's own custom
    // properties fails dependency tracking here: the value is computed once and
    // then never updated when the dependency changes, which leaves the derived
    // sizes stuck (seen as NaN) and every pane zero-sized. `root.pad` tracks
    // correctly. Keep the prefix on every internal reference below.
    readonly property bool landscape: width >= height
    readonly property real unit: Math.min(width, height)
    // Slimmer nav rail so the map/content gets more room (Android Auto keeps its
    // chrome thin).
    readonly property real railT: Math.round((root.landscape ? width : height) * (root.landscape ? 0.11 : 0.1))
    readonly property real pad: Math.round(root.unit * 0.025)
    readonly property real rad: Math.round(root.unit * 0.03)

    // content box (next to / above the rail)
    readonly property real cx: root.landscape ? root.railT : 0
    readonly property real cw: root.landscape ? width - root.railT : width
    readonly property real ch: root.landscape ? height : height - root.railT

    // content fills the whole box now that there's no status bar
    readonly property real sx: root.cx + root.pad
    readonly property real sy: root.pad
    readonly property real sw: root.cw - root.pad * 2
    readonly property real sh: root.ch - root.pad * 2

    // which rail item is active: "maps"/"media" show the dashboard, "apps" the
    // app launcher, "phone" the phone screen.
    readonly property bool onDash: activeApp === "maps" || activeApp === "media"

    // Map dominates (like Android Auto); the now-playing pane is a thin strip.
    readonly property real mapSplit: 0.76
    readonly property real mapW: root.landscape ? Math.round(root.sw * root.mapSplit - root.pad / 2) : root.sw
    readonly property real mapH: root.landscape ? root.sh : Math.round(root.sh * root.mapSplit - root.pad / 2)
    readonly property real medX: root.landscape ? root.sx + root.mapW + root.pad : root.sx
    readonly property real medY: root.landscape ? root.sy : root.sy + root.mapH + root.pad
    readonly property real medW: root.landscape ? root.sw - root.mapW - root.pad : root.sw
    readonly property real medH: root.landscape ? root.sh : root.sh - root.mapH - root.pad

    // --- theme (night mode comes from the HU's sensor, via CarScreen) ---
    readonly property bool night: carScreen.nightMode
    readonly property color bg:       night ? "#0b0f14" : "#e9eef3"
    readonly property color surface:  night ? "#161d27" : "#ffffff"
    readonly property color surface2: night ? "#202a37" : "#dde6ef"
    readonly property color fg:       night ? "#eef3f8" : "#0b1620"
    readonly property color dim:      night ? "#90a4b8" : "#5a6b7b"
    readonly property color accent:   "#4c8dff"
    readonly property color accent2:  "#34c759"

    // --- interactive state ---
    property string activeApp: "maps"
    property bool playing: true
    // Tap the map to expand it full-screen (media collapses to a bottom
    // minibar); tap the minibar to return to split view (Android Auto style).
    property bool mapExpanded: false
    readonly property real minibarH: Math.round(unit * 0.12)
    // destination search / navigation (driven by MapPane, id realMap)
    property bool searching: false
    property string searchText: ""
    // Saved-place picking: "home"/"work" while the search is choosing that
    // place (the next result tapped is saved instead of navigated to).
    property string pickPlace: ""
    onSearchingChanged: if (!searching) { pickPlace = ""; chargerMode = false }
    // The search overlay is listing EV charging stations instead of geocoding.
    property bool chargerMode: false
    function openChargers() {
        root.searchText = ""; root.pickPlace = ""
        root.chargerMode = true; root.searching = true
        if (root.navMap) root.navMap.findChargers()
    }
    onPickPlaceChanged: if (root.navMap) root.navMap.geocodeModel = [] // no stale results
    readonly property bool havePlaces: typeof carScreen !== "undefined" && carScreen
                                       && carScreen.places !== undefined
    // Navigate to a saved/recent place {label, lat, lon} and close the search.
    function goToPlace(pl, bumpRecent) {
        if (!pl || !root.navMap) return
        root.navMap.navigateTo(QtPositioning.coordinate(pl.lat, pl.lon), pl.label)
        if (bumpRecent) carScreen.addRecentPlace(pl.label, pl.lat, pl.lon) // move to the top
        root.searching = false; root.searchText = ""
    }
    property bool showQueue: false   // the play-queue / track-list overlay
    readonly property var navMap: (realMap.status === Loader.Ready && realMap.item) ? realMap.item : null
    readonly property bool navigating: navMap ? navMap.navigating : false

    // Live media backend (MPRIS), exposed by CarScreen as `media`. Falls back to
    // placeholder content when no player is present.
    readonly property bool hasMedia: (typeof media !== "undefined") && !!media && media.hasPlayer
    Binding { target: root; property: "playing"; value: root.hasMedia ? media.playing : root.playing; when: root.hasMedia }

    color: bg

    Behavior on color { ColorAnimation { duration: 250 } }

    // ---------------------------------------------------------------- icon
    // One reusable glyph painter, selected by name. Immediate render target so
    // it paints synchronously under the offscreen render control.
    Component {
        id: iconPainter
        Canvas {
            property string name: "maps"
            property color col: root.fg
            renderStrategy: Canvas.Immediate
            renderTarget: Canvas.Image
            antialiasing: true
            onColChanged: requestPaint()
            Component.onCompleted: requestPaint()
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                var w = width, h = height;
                ctx.strokeStyle = col;
                ctx.fillStyle = col;
                ctx.lineWidth = Math.max(2, w * 0.08);
                ctx.lineJoin = "round";
                ctx.lineCap = "round";
                if (name === "maps") {
                    ctx.beginPath();
                    ctx.arc(w * 0.5, h * 0.4, w * 0.22, Math.PI, 2 * Math.PI);
                    ctx.lineTo(w * 0.5, h * 0.82);
                    ctx.closePath();
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.arc(w * 0.5, h * 0.4, w * 0.08, 0, 2 * Math.PI);
                    ctx.fill();
                } else if (name === "media") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.38, h * 0.72);
                    ctx.lineTo(w * 0.38, h * 0.28);
                    ctx.lineTo(w * 0.72, h * 0.20);
                    ctx.lineTo(w * 0.72, h * 0.62);
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.arc(w * 0.30, h * 0.72, w * 0.10, 0, 2 * Math.PI);
                    ctx.arc(w * 0.64, h * 0.62, w * 0.10, 0, 2 * Math.PI);
                    ctx.fill();
                } else if (name === "phone") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.28, h * 0.22);
                    ctx.quadraticCurveTo(w * 0.22, h * 0.28, w * 0.30, h * 0.46);
                    ctx.quadraticCurveTo(w * 0.42, h * 0.66, w * 0.60, h * 0.72);
                    ctx.quadraticCurveTo(w * 0.74, h * 0.76, w * 0.78, h * 0.66);
                    ctx.lineTo(w * 0.62, h * 0.54);
                    ctx.lineTo(w * 0.52, h * 0.60);
                    ctx.quadraticCurveTo(w * 0.44, h * 0.54, w * 0.40, h * 0.46);
                    ctx.lineTo(w * 0.46, h * 0.36);
                    ctx.closePath();
                    ctx.stroke();
                } else if (name === "apps") {
                    var r = w * 0.09, g = w * 0.20;
                    for (var i = 0; i < 2; i++)
                        for (var j = 0; j < 2; j++) {
                            ctx.beginPath();
                            ctx.arc(w * 0.5 + (i === 0 ? -g : g), h * 0.5 + (j === 0 ? -g : g), r, 0, 2 * Math.PI);
                            ctx.fill();
                        }
                } else if (name === "back") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.60, h * 0.25);
                    ctx.lineTo(w * 0.38, h * 0.5);
                    ctx.lineTo(w * 0.60, h * 0.75);
                    ctx.stroke();
                } else if (name === "play") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.36, h * 0.26);
                    ctx.lineTo(w * 0.36, h * 0.74);
                    ctx.lineTo(w * 0.74, h * 0.5);
                    ctx.closePath();
                    ctx.fill();
                } else if (name === "pause") {
                    ctx.fillRect(w * 0.34, h * 0.26, w * 0.12, h * 0.48);
                    ctx.fillRect(w * 0.54, h * 0.26, w * 0.12, h * 0.48);
                } else if (name === "prev" || name === "next") {
                    var s = name === "next" ? 1 : -1, cxp = w * 0.5;
                    ctx.beginPath();
                    ctx.moveTo(cxp - s * w * 0.18, h * 0.28);
                    ctx.lineTo(cxp + s * w * 0.06, h * 0.5);
                    ctx.lineTo(cxp - s * w * 0.18, h * 0.72);
                    ctx.closePath();
                    ctx.fill();
                    ctx.fillRect(cxp + s * w * 0.12 - (s > 0 ? 0 : w * 0.08), h * 0.28, w * 0.08, h * 0.44);
                } else if (name === "moon") {
                    ctx.beginPath();
                    ctx.arc(w * 0.5, h * 0.5, w * 0.26, 0, 2 * Math.PI);
                    ctx.fill();
                    ctx.fillStyle = root.surface;
                    ctx.beginPath();
                    ctx.arc(w * 0.60, h * 0.42, w * 0.24, 0, 2 * Math.PI);
                    ctx.fill();
                } else if (name === "hangup") {
                    // the phone handset, rotated down (ending a call)
                    ctx.save();
                    ctx.translate(w * 0.5, h * 0.5);
                    ctx.rotate(135 * Math.PI / 180);
                    ctx.translate(-w * 0.5, -h * 0.5);
                    ctx.beginPath();
                    ctx.moveTo(w * 0.28, h * 0.22);
                    ctx.quadraticCurveTo(w * 0.22, h * 0.28, w * 0.30, h * 0.46);
                    ctx.quadraticCurveTo(w * 0.42, h * 0.66, w * 0.60, h * 0.72);
                    ctx.quadraticCurveTo(w * 0.74, h * 0.76, w * 0.78, h * 0.66);
                    ctx.lineTo(w * 0.62, h * 0.54);
                    ctx.lineTo(w * 0.52, h * 0.60);
                    ctx.quadraticCurveTo(w * 0.44, h * 0.54, w * 0.40, h * 0.46);
                    ctx.lineTo(w * 0.46, h * 0.36);
                    ctx.closePath();
                    ctx.fill();
                    ctx.restore();
                } else if (name === "mute" || name === "unmute") {
                    // microphone capsule (two arcs + sides, no roundedRect in Qt5.6)
                    ctx.beginPath();
                    ctx.arc(w * 0.5, h * 0.32, w * 0.08, Math.PI, 2 * Math.PI);
                    ctx.lineTo(w * 0.58, h * 0.46);
                    ctx.arc(w * 0.5, h * 0.46, w * 0.08, 0, Math.PI);
                    ctx.closePath();
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.arc(w * 0.5, h * 0.50, w * 0.17, 0.15 * Math.PI, 0.85 * Math.PI);
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.moveTo(w * 0.5, h * 0.67);
                    ctx.lineTo(w * 0.5, h * 0.80);
                    ctx.stroke();
                    if (name === "mute") { // a slash through it
                        ctx.beginPath();
                        ctx.moveTo(w * 0.26, h * 0.20);
                        ctx.lineTo(w * 0.74, h * 0.80);
                        ctx.stroke();
                    }
                } else if (name === "backspace") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.34, h * 0.30);
                    ctx.lineTo(w * 0.78, h * 0.30);
                    ctx.lineTo(w * 0.78, h * 0.70);
                    ctx.lineTo(w * 0.34, h * 0.70);
                    ctx.closePath();
                    ctx.moveTo(w * 0.34, h * 0.30);
                    ctx.lineTo(w * 0.16, h * 0.5);
                    ctx.lineTo(w * 0.34, h * 0.70);
                    ctx.stroke();
                } else if (name === "home") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.16, h * 0.48);
                    ctx.lineTo(w * 0.5, h * 0.18);
                    ctx.lineTo(w * 0.84, h * 0.48);
                    ctx.moveTo(w * 0.26, h * 0.40);
                    ctx.lineTo(w * 0.26, h * 0.80);
                    ctx.lineTo(w * 0.74, h * 0.80);
                    ctx.lineTo(w * 0.74, h * 0.40);
                    ctx.stroke();
                    ctx.fillRect(w * 0.43, h * 0.58, w * 0.14, h * 0.22);
                } else if (name === "work") {
                    ctx.beginPath();
                    ctx.rect(w * 0.16, h * 0.34, w * 0.68, h * 0.44);
                    ctx.moveTo(w * 0.38, h * 0.34);
                    ctx.lineTo(w * 0.38, h * 0.22);
                    ctx.lineTo(w * 0.62, h * 0.22);
                    ctx.lineTo(w * 0.62, h * 0.34);
                    ctx.moveTo(w * 0.16, h * 0.54);
                    ctx.lineTo(w * 0.84, h * 0.54);
                    ctx.stroke();
                } else if (name === "bolt") {
                    ctx.beginPath();
                    ctx.moveTo(w * 0.58, h * 0.10);
                    ctx.lineTo(w * 0.24, h * 0.56);
                    ctx.lineTo(w * 0.48, h * 0.56);
                    ctx.lineTo(w * 0.40, h * 0.90);
                    ctx.lineTo(w * 0.76, h * 0.42);
                    ctx.lineTo(w * 0.52, h * 0.42);
                    ctx.closePath();
                    ctx.fill();
                } else if (name === "recent") {
                    ctx.beginPath();
                    ctx.arc(w * 0.5, h * 0.5, w * 0.32, 0, 2 * Math.PI);
                    ctx.moveTo(w * 0.5, h * 0.30);
                    ctx.lineTo(w * 0.5, h * 0.5);
                    ctx.lineTo(w * 0.64, h * 0.60);
                    ctx.stroke();
                }
            }
        }
    }

    // --------------------------------------------------------- navigation rail
    Rectangle {
        id: rail
        color: root.surface
        x: 0
        y: root.landscape ? 0 : root.height - root.railT
        width: root.landscape ? root.railT : root.width
        height: root.landscape ? root.height : root.railT

        ListModel {
            id: navModel
            ListElement { key: "maps"; icon: "maps" }
            ListElement { key: "media"; icon: "media" }
            ListElement { key: "phone"; icon: "phone" }
            ListElement { key: "apps"; icon: "apps" }
        }

        Grid {
            anchors.centerIn: parent
            columns: root.landscape ? 1 : navModel.count
            rows: root.landscape ? navModel.count : 1
            columnSpacing: root.pad
            rowSpacing: root.pad * 1.3
            Repeater {
                model: navModel
                Rectangle {
                    width: Math.round(root.railT * 0.72)
                    height: width
                    radius: width * 0.28
                    color: root.activeApp === key ? root.accent : "transparent"
                    Loader {
                        id: navIcon
                        anchors.centerIn: parent
                        width: parent.width * 0.56
                        height: width
                        sourceComponent: iconPainter
                        onLoaded: item.name = icon
                    }
                    Binding {
                        target: navIcon.item; property: "col"
                        value: root.activeApp === key ? "white" : root.dim
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            root.activeApp = key
                            if (key === "maps") root.mapExpanded = true       // focus the map
                            else if (key === "media") root.mapExpanded = false // split, show now-playing
                        }
                    }
                }
            }
        }
    }

    // (No top status bar: the head unit shows the clock itself, and night mode
    // follows the car's sensor. The content fills the whole area.)

    // ---------------------------------------------------------------- map pane
    Rectangle {
        id: mapPane
        visible: root.onDash
        x: root.sx; y: root.sy
        width: root.mapExpanded ? root.sw : root.mapW
        // Full-screen: the map fills the area but leaves a strip at the bottom for
        // the now-playing status bar, so the music bar sits UNDER the map rather
        // than overlapping it.
        height: root.mapExpanded ? root.sh - root.minibarH - root.pad : root.mapH
        radius: root.rad
        color: root.surface2
        clip: true
        Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }
        Behavior on height { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }

        // Live OSM map: the self-contained TileMap (tiles + GPS + Nominatim
        // search + OSRM routing). QtLocation's osm plugin fails to load on this
        // device, so TileMap is the single map backend on phone and sim alike.
        Loader {
            id: realMap
            anchors.fill: parent
            source: Qt.resolvedUrl("TileMap.qml")
            onStatusChanged: root.mlog("map status=" + status + (status === Loader.Error ? " ERROR" : ""))
        }
        // Tap anywhere on the map (in split view) to expand it full-screen.
        MouseArea {
            anchors.fill: parent
            enabled: !root.mapExpanded
            onClicked: root.mapExpanded = true
        }
        // search bar (shown when not navigating)
        Rectangle {
            visible: !root.navigating
            x: root.pad; y: root.pad
            width: Math.min(parent.width - root.pad * 2, root.unit * 0.46)
            height: Math.round(root.unit * 0.082)
            radius: height / 2
            color: root.surface
            Row {
                anchors.left: parent.left; anchors.leftMargin: parent.height * 0.42
                anchors.verticalCenter: parent.verticalCenter
                spacing: parent.height * 0.32
                Canvas {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.parent.height * 0.44; height: width
                    renderStrategy: Canvas.Immediate; renderTarget: Canvas.Image; antialiasing: true
                    onPaint: {
                        var ctx = getContext("2d"); ctx.reset(); var w = width, h = height;
                        ctx.strokeStyle = root.dim; ctx.lineWidth = Math.max(2, w * 0.1); ctx.lineCap = "round";
                        ctx.beginPath(); ctx.arc(w * 0.42, h * 0.42, w * 0.3, 0, 2 * Math.PI); ctx.stroke();
                        ctx.beginPath(); ctx.moveTo(w * 0.64, h * 0.64); ctx.lineTo(w * 0.9, h * 0.9); ctx.stroke();
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.dim; font.pixelSize: parent.parent.height * 0.34
                    text: "Search destination"
                }
            }
            MouseArea { anchors.fill: parent; onClicked: { root.searching = true; root.mapExpanded = true } }
        }

        // Map controls live on the LEFT edge (like Google Maps), stacked below the
        // search pill / turn card so they never overlap it.
        readonly property real ctrlTop: root.pad + Math.round(root.unit * 0.12) + root.pad

        // traffic toggle: shown only when a TomTom key is configured.
        // Flips the live traffic overlay on/off; the Controller persists the state.
        Rectangle {
            id: trafficBtn
            visible: typeof carScreen !== "undefined" && carScreen && carScreen.trafficKey !== ""
            property bool on: visible && carScreen.trafficEnabled
            anchors.left: parent.left; anchors.top: parent.top
            anchors.leftMargin: root.pad; anchors.topMargin: mapPane.ctrlTop
            width: Math.round(root.unit * 0.056); height: width; radius: width * 0.28
            color: on ? root.accent : root.surface
            Canvas {
                anchors.centerIn: parent
                width: parent.width * 0.5; height: parent.height * 0.62
                renderStrategy: Canvas.Immediate; renderTarget: Canvas.Image; antialiasing: true
                property bool on: trafficBtn.on
                onOnChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d"); ctx.reset(); var w = width, h = height
                    ctx.strokeStyle = trafficBtn.on ? "white" : root.dim
                    ctx.lineWidth = Math.max(1.5, w * 0.1)
                    ctx.strokeRect(w * 0.2, 0, w * 0.6, h) // light housing
                    var cx = w * 0.5, r = w * 0.12
                    var cols = trafficBtn.on ? ["#ff3b30", "#ffcc00", "#34c759"]
                                             : [root.dim, root.dim, root.dim]
                    for (var i = 0; i < 3; i++) {
                        ctx.beginPath(); ctx.fillStyle = cols[i]
                        ctx.arc(cx, h * (0.22 + i * 0.28), r, 0, 2 * Math.PI); ctx.fill()
                    }
                }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: if (typeof carScreen !== "undefined" && carScreen)
                               carScreen.trafficEnabled = !carScreen.trafficEnabled
            }
        }

        // compass toggle (left edge, below traffic): switches the map between
        // north-up (fixed) and heading-up (rotates with the car), like Google
        // Maps. The needle rotates to point to actual north on screen.
        Rectangle {
            id: compassBtn
            visible: root.navMap !== null
            property real mapRot: (root.navMap && root.navMap.headingUp) ? -root.navMap._dispHeading : 0
            property bool headingUp: root.navMap ? root.navMap.headingUp : false
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: root.pad
            // sit below the traffic button when it's showing, else at the top control row.
            anchors.topMargin: mapPane.ctrlTop + (trafficBtn.visible ? trafficBtn.height + root.pad : 0)
            width: Math.round(root.unit * 0.056); height: width; radius: width / 2
            color: root.surface
            Canvas {
                id: compassFace
                anchors.centerIn: parent
                width: parent.width * 0.74; height: width
                renderStrategy: Canvas.Immediate; renderTarget: Canvas.Image; antialiasing: true
                rotation: compassBtn.mapRot
                Behavior on rotation { RotationAnimation { duration: 220; direction: RotationAnimation.Shortest } }
                property bool hu: compassBtn.headingUp
                onHuChanged: requestPaint()
                onPaint: {
                    var ctx = getContext("2d"); ctx.reset()
                    var w = width, h = height, cx = w / 2, cy = h / 2
                    // north half (red), south half (grey) needle
                    ctx.beginPath(); ctx.moveTo(cx, h * 0.08); ctx.lineTo(w * 0.72, cy); ctx.lineTo(w * 0.28, cy)
                    ctx.closePath(); ctx.fillStyle = "#ff3b30"; ctx.fill()
                    ctx.beginPath(); ctx.moveTo(cx, h * 0.92); ctx.lineTo(w * 0.72, cy); ctx.lineTo(w * 0.28, cy)
                    ctx.closePath(); ctx.fillStyle = root.dim; ctx.fill()
                }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: if (typeof carScreen !== "undefined" && carScreen)
                               carScreen.mapHeadingUp = !carScreen.mapHeadingUp
            }
        }

        // current speed (bottom-right), like Google Maps: a round badge with the
        // live GPS speed. Hidden until we have a valid fix.
        Rectangle {
            id: speedBadge
            visible: root.navMap !== null && root.navMap.speedKmh >= 0
            anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.margins: root.pad
            width: Math.round(root.unit * 0.11); height: width; radius: width / 2
            // Over the posted limit: the badge turns red.
            readonly property bool speeding: root.navMap !== null && root.navMap.speedLimit > 0
                                             && root.navMap.speedKmh > root.navMap.speedLimit
            color: speeding ? "#e5392f" : root.surface
            border.width: Math.max(1, Math.round(width * 0.04)); border.color: speeding ? "#e5392f" : root.dim
            Column {
                anchors.centerIn: parent; spacing: 0
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.navMap ? root.navMap.speedKmh : 0
                    color: speedBadge.speeding ? "white" : root.fg
                    font.bold: true; font.pixelSize: Math.round(speedBadge.width * 0.4)
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "km/h"; color: speedBadge.speeding ? "#ffe1de" : root.dim
                    font.pixelSize: Math.round(speedBadge.width * 0.18)
                }
            }
        }

        // posted speed limit (left of the speedometer): a European-style round
        // sign, white with a red ring. Hidden when the road's limit is unknown.
        Rectangle {
            id: limitSign
            visible: speedBadge.visible && root.navMap.speedLimit > 0
            anchors.right: speedBadge.left; anchors.rightMargin: root.pad * 0.5
            anchors.verticalCenter: speedBadge.verticalCenter
            width: Math.round(speedBadge.width * 0.86); height: width; radius: width / 2
            color: "white"
            border.width: Math.round(width * 0.12); border.color: "#e5392f"
            Text {
                anchors.centerIn: parent
                text: root.navMap ? root.navMap.speedLimit : ""
                color: "#111"; font.bold: true
                font.pixelSize: Math.round(limitSign.width * (text.length > 2 ? 0.34 : 0.42))
            }
        }

        // turn card (shown when navigating): a compact green maneuver card like
        // Android Auto — arrow + distance + instruction. Route controls live in
        // the trip card at the bottom-left.
        Rectangle {
            id: turnCard
            visible: root.navigating
            x: root.pad; y: root.pad
            width: Math.min(parent.width - root.pad * 2, root.unit * 0.52)
            height: root.unit * 0.12
            radius: root.rad * 0.7
            color: root.accent2
            Row {
                anchors.fill: parent
                anchors.margins: parent.height * 0.16
                spacing: parent.height * 0.18
                Canvas {
                    id: turnArrow
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.height; height: width
                    renderStrategy: Canvas.Immediate; renderTarget: Canvas.Image; antialiasing: true
                    // QtLocation RouteManeuver.Direction: 2-5 right family, 8-11 left,
                    // 6/7 u-turn, 0/1 straight.
                    property int dir: root.navMap ? root.navMap.maneuverDir : 1
                    onDirChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d"); ctx.reset(); var w = width, h = height;
                        ctx.strokeStyle = "white"; ctx.fillStyle = "white";
                        ctx.lineWidth = Math.max(3, w * 0.12); ctx.lineJoin = "round"; ctx.lineCap = "round";
                        var right = dir >= 2 && dir <= 5, left = dir >= 8 && dir <= 11, uturn = dir === 6 || dir === 7;
                        if (uturn) {
                            ctx.beginPath(); ctx.moveTo(w * 0.64, h * 0.82); ctx.lineTo(w * 0.64, h * 0.46);
                            ctx.arc(w * 0.46, h * 0.46, w * 0.18, 0, Math.PI, true); ctx.lineTo(w * 0.28, h * 0.6); ctx.stroke();
                            ctx.beginPath(); ctx.moveTo(w * 0.28, h * 0.44); ctx.lineTo(w * 0.1, h * 0.6);
                            ctx.lineTo(w * 0.28, h * 0.76); ctx.closePath(); ctx.fill();
                        } else if (right || left) {
                            if (left) { ctx.translate(w, 0); ctx.scale(-1, 1); } // mirror for a left turn
                            ctx.beginPath(); ctx.moveTo(w * 0.3, h * 0.82); ctx.lineTo(w * 0.3, h * 0.5);
                            ctx.quadraticCurveTo(w * 0.3, h * 0.3, w * 0.58, h * 0.3); ctx.lineTo(w * 0.76, h * 0.3); ctx.stroke();
                            ctx.beginPath(); ctx.moveTo(w * 0.6, h * 0.14); ctx.lineTo(w * 0.82, h * 0.3);
                            ctx.lineTo(w * 0.6, h * 0.46); ctx.closePath(); ctx.fill();
                        } else {
                            ctx.beginPath(); ctx.moveTo(w * 0.5, h * 0.82); ctx.lineTo(w * 0.5, h * 0.28); ctx.stroke();
                            ctx.beginPath(); ctx.moveTo(w * 0.3, h * 0.44); ctx.lineTo(w * 0.5, h * 0.16);
                            ctx.lineTo(w * 0.7, h * 0.44); ctx.closePath(); ctx.fill();
                        }
                    }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - turnArrow.width - parent.spacing
                    Text {
                        color: "white"; font.pixelSize: turnCard.height * 0.3; font.bold: true
                        text: root.navMap ? root.navMap.nextDistance : ""
                    }
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        color: "#eafff1"; font.pixelSize: turnCard.height * 0.2
                        text: root.navMap ? (root.navMap.nextInstruction || root.navMap.summary) : ""
                    }
                }
            }
        }

        // Low battery at arrival (EV, <= 10%): a red pill above the trip card that
        // opens nearby charging stations.
        Rectangle {
            id: lowBattCard
            readonly property int pct: root.navMap ? root.navMap.evArrivalPct : -1
            visible: root.navigating && pct >= 0 && pct <= 10
            anchors.left: tripCard.left; anchors.bottom: tripCard.top; anchors.bottomMargin: root.pad * 0.5
            height: Math.round(root.unit * 0.075)
            width: lowBattRow.width + height * 0.7
            radius: height / 2
            color: lowBattArea.pressed ? "#c62d24" : "#e5392f"
            Row {
                id: lowBattRow
                anchors.centerIn: parent
                spacing: lowBattCard.height * 0.15
                Loader {
                    anchors.verticalCenter: parent.verticalCenter
                    width: lowBattCard.height * 0.62; height: width
                    sourceComponent: iconPainter
                    onLoaded: { item.name = "bolt"; item.col = "white" }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: "white"; font.bold: true; font.pixelSize: lowBattCard.height * 0.36
                    text: "Arriving with " + lowBattCard.pct + "%"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#ffe1de"; font.pixelSize: lowBattCard.height * 0.32
                    text: "·  Find chargers"
                }
            }
            MouseArea { id: lowBattArea; anchors.fill: parent; onClicked: root.openChargers() }
        }

        // trip card (shown when navigating): a compact dark card like Android
        // Auto — remaining time (headline), then "distance · arrival", plus a
        // small button to end navigation.
        Rectangle {
            id: tripCard
            visible: root.navigating
            anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: root.pad
            height: Math.round(root.unit * 0.115)
            width: tripRow.width + height * 0.5
            radius: root.rad * 0.7
            color: root.night ? "#d9161d27" : "#e6161d27"   // dark translucent, both themes
            Row {
                id: tripRow
                anchors.centerIn: parent
                spacing: tripCard.height * 0.28
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        color: "white"; font.pixelSize: tripCard.height * 0.36; font.bold: true
                        // remaining travel time, from the live summary ("12 km · 18 min")
                        text: {
                            if (!root.navMap || !root.navMap.summary) return "--"
                            var p = root.navMap.summary.split("·")
                            return p.length > 1 ? p[1].trim() : root.navMap.summary
                        }
                    }
                    Text {
                        color: "#b9c6d4"; font.pixelSize: tripCard.height * 0.2
                        text: {
                            if (!root.navMap) return ""
                            var p = root.navMap.summary ? root.navMap.summary.split("·") : []
                            var dist = p.length > 0 ? p[0].trim() : ""
                            return dist + (root.navMap.etaText ? "  ·  " + root.navMap.etaText : "")
                        }
                    }
                }
                // battery charge at arrival (electric cars only, when the head unit
                // reports the car's energy). Shows a small battery + the estimated %.
                Row {
                    visible: root.navMap && root.navMap.evArrivalPct >= 0
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: tripCard.height * 0.1
                    property int pct: root.navMap ? root.navMap.evArrivalPct : 0
                    property color battColor: pct <= 10 ? "#ff3b30" : (pct <= 20 ? "#ffcc00" : "#34c759")
                    Canvas {
                        anchors.verticalCenter: parent.verticalCenter
                        width: tripCard.height * 0.46; height: tripCard.height * 0.26
                        renderStrategy: Canvas.Immediate; renderTarget: Canvas.Image; antialiasing: true
                        property int pct: parent.pct
                        property color col: parent.battColor
                        onPctChanged: requestPaint()
                        onColChanged: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d"); ctx.reset()
                            var w = width, h = height, lw = Math.max(1, w * 0.05), bw = w * 0.85
                            ctx.strokeStyle = "white"; ctx.lineWidth = lw
                            ctx.strokeRect(lw, lw, bw - lw * 2, h - lw * 2)
                            ctx.fillStyle = "white"; ctx.fillRect(bw, h * 0.32, w * 0.12, h * 0.36)
                            ctx.fillStyle = col
                            var innerW = (bw - lw * 4) * Math.max(0, Math.min(1, pct / 100))
                            ctx.fillRect(lw * 2, lw * 2, innerW, h - lw * 4)
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        color: parent.battColor; font.bold: true
                        font.pixelSize: tripCard.height * 0.24
                        text: parent.pct + "%"
                    }
                }
                // end navigation
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: tripCard.height * 0.6; height: width; radius: width / 2
                    color: "#33ffffff"
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: parent.height * 0.5; text: "✕" }
                    MouseArea { anchors.fill: parent; onClicked: if (root.navMap) root.navMap.clearRoute() }
                }
            }
        }
    }

    // ----------------------------------------------------------- now playing
    Rectangle {
        id: mediaPane
        visible: root.onDash
        // Split view: the smaller pane beside/below the map. Expanded view: a
        // slim minibar pinned to the bottom, overlaying the full-screen map.
        x: root.mapExpanded ? root.sx : root.medX
        y: root.mapExpanded ? root.sy + root.sh - root.minibarH : root.medY
        width: root.mapExpanded ? root.sw : root.medW
        height: root.mapExpanded ? root.minibarH : root.medH
        radius: root.rad
        color: root.surface
        clip: true
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }
        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }
        Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }
        Behavior on height { NumberAnimation { duration: 220; easing.type: Easing.InOutQuad } }

        // "strip" = a short, wide media bar (map-dominant portrait layout);
        // otherwise a tall narrow column (landscape). All sizes scale off the
        // pane's OWN short edge (uh) so nothing overflows a short pane.
        readonly property bool strip: width > height * 1.4
        readonly property real uh: strip ? height : width
        readonly property real m: root.pad
        // Real playback position from the MPRIS backend (0..1); 0 when no player
        // or the player reports no track length.
        readonly property real progress: root.hasMedia ? Math.max(0, Math.min(1, media.progress)) : 0

        // In expanded view, tapping the minibar (anywhere but the transport
        // buttons, which have their own MouseAreas on top) returns to split view.
        MouseArea {
            anchors.fill: parent
            enabled: root.mapExpanded
            onClicked: root.mapExpanded = false
        }

        // album art
        Rectangle {
            id: art
            x: mediaPane.m; y: mediaPane.m
            width: mediaPane.strip ? mediaPane.height - mediaPane.m * 2
                                   : Math.min(parent.width - mediaPane.m * 2, parent.height * 0.34)
            height: width
            radius: root.rad * 0.8
            gradient: Gradient {
                GradientStop { position: 0.0; color: root.accent }
                GradientStop { position: 1.0; color: "#7b5cff" }
            }
            Loader {
                anchors.centerIn: parent
                width: parent.width * 0.4; height: width
                sourceComponent: iconPainter
                onLoaded: { item.name = "media"; item.col = "white"; }
            }
            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                source: (root.hasMedia && (media.artUrl.indexOf("file:") === 0
                                           || media.artUrl.indexOf("http") === 0)) ? media.artUrl : ""
                visible: status === Image.Ready
            }
            // Tap the artwork to open the play queue (track list) of the current
            // player, so a passenger can jump to any song in it.
            MouseArea {
                anchors.fill: parent
                enabled: root.hasMedia
                onClicked: { media.refreshQueue(); root.showQueue = true }
            }
            // "list" hint badge in a corner so it reads as tappable.
            Rectangle {
                visible: root.hasMedia
                anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.margins: parent.width * 0.06
                width: parent.width * 0.2; height: width; radius: height * 0.3
                color: "#99000000"
                Column {
                    anchors.centerIn: parent; spacing: Math.max(1, parent.height * 0.1)
                    Repeater {
                        model: 3
                        Rectangle { width: parent.parent.width * 0.5; height: Math.max(1.5, parent.parent.height * 0.1)
                                    radius: height / 2; color: "white" }
                    }
                }
            }
        }

        // transport controls. A container Item with an EXPLICIT size (so its
        // right/left edges resolve cleanly for anchoring) holding a centred row
        // of buttons. Right-aligned in strip mode, bottom-centred in column mode.
        Item {
            id: controls
            property real btn: mediaPane.uh * (mediaPane.strip ? 0.36 : 0.22)
            property real sp: mediaPane.uh * (mediaPane.strip ? 0.08 : 0.055)
            width: btn * 3.2 + sp * 2
            height: btn * 1.2
            // Explicit x/y (no ternary anchors, which conflict): right-centred in
            // strip mode, bottom-centred in column mode.
            x: mediaPane.strip ? mediaPane.width - width - mediaPane.m * 1.5
                               : (mediaPane.width - width) / 2
            y: mediaPane.strip ? (mediaPane.height - height) / 2
                               : mediaPane.height - height - mediaPane.m * 1.6
            Row {
                anchors.centerIn: parent
                spacing: controls.sp
                Repeater {
                    model: [ "prev", "playpause", "next" ]
                    Rectangle {
                        property bool primary: modelData === "playpause"
                        anchors.verticalCenter: parent.verticalCenter
                        width: primary ? controls.btn * 1.2 : controls.btn
                        height: width; radius: width / 2
                        color: primary ? root.accent : root.surface2
                        Loader {
                            id: ctlIcon
                            anchors.centerIn: parent
                            width: parent.width * 0.58; height: width
                            sourceComponent: iconPainter
                            onLoaded: item.col = primary ? "white" : root.fg
                        }
                        Binding {
                            target: ctlIcon.item; property: "name"
                            value: primary ? (root.playing ? "pause" : "play") : modelData
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (root.hasMedia) {
                                    if (modelData === "playpause") media.playPause()
                                    else if (modelData === "next") media.next()
                                    else if (modelData === "prev") media.previous()
                                } else if (primary) {
                                    root.playing = !root.playing
                                }
                            }
                        }
                    }
                }
            }
        }

        // title / artist: beside the art, filling the space up to the controls
        // (strip) or below the art (column). Explicit x/y/width (no ternary
        // anchors) so it never collapses.
        Column {
            id: info
            // strip: beside the art, up to the controls. column: below the art,
            // full width.
            x: mediaPane.strip ? art.x + art.width + mediaPane.m * 1.2 : mediaPane.m
            y: mediaPane.strip ? (mediaPane.height - height) / 2
                               : art.y + art.height + mediaPane.m
            width: mediaPane.strip ? controls.x - x - mediaPane.m * 1.2
                                   : mediaPane.width - mediaPane.m * 2
            spacing: mediaPane.uh * 0.04
            Text {
                width: parent.width; elide: Text.ElideRight
                color: root.fg; font.bold: true
                font.pixelSize: mediaPane.uh * (mediaPane.strip ? 0.2 : 0.1)
                text: root.hasMedia ? (media.title !== "" ? media.title : "Unknown track")
                                    : (root.playing ? "Golden Hour" : "Paused")
            }
            Text {
                width: parent.width; elide: Text.ElideRight
                color: root.dim
                font.pixelSize: mediaPane.uh * (mediaPane.strip ? 0.14 : 0.07)
                text: root.hasMedia ? (media.artist + (media.album !== "" ? " · " + media.album : ""))
                                    : "Sailfish FM · Drive playlist"
            }
        }

        // progress: a thin line pinned to the bottom edge.
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: mediaPane.strip ? art.width + mediaPane.m * 2 : mediaPane.m
            anchors.rightMargin: mediaPane.m
            anchors.bottomMargin: mediaPane.m * 0.7
            height: Math.max(3, mediaPane.uh * 0.02); radius: height / 2
            color: root.surface2
            Rectangle {
                width: parent.width * mediaPane.progress; height: parent.height
                radius: height / 2; color: root.accent
            }
        }
    }

    // ----------------------------------------------------------------- apps
    // A scrollable grid of installed apps; tapping one launches it on the phone.
    Rectangle {
        id: appsView
        visible: root.activeApp === "apps"
        x: root.sx; y: root.sy; width: root.sw; height: root.sh
        radius: root.rad
        color: root.surface
        clip: true

        Text {
            id: appsTitle
            x: root.pad * 1.3; y: root.pad
            color: root.fg; font.bold: true; font.pixelSize: root.unit * 0.05
            text: "Apps"
        }
        Flickable {
            anchors.fill: parent
            anchors.topMargin: appsTitle.y + appsTitle.height + root.pad
            anchors.margins: root.pad
            contentHeight: flow.height
            clip: true
            Flow {
                id: flow
                width: parent.width
                spacing: root.pad
                Repeater {
                    model: (typeof appList !== "undefined" && appList) ? appList.apps : []
                    Rectangle {
                        width: Math.round((flow.width - root.pad * 2) / 3)
                        height: root.unit * 0.14
                        radius: root.rad * 0.8
                        color: tileArea.pressed ? root.accent : root.surface2
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - root.pad
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight
                            color: tileArea.pressed ? "white" : root.fg
                            font.pixelSize: root.unit * 0.03
                            text: modelData.name + (modelData.android ? "  ·  Android" : "")
                        }
                        MouseArea {
                            id: tileArea
                            anchors.fill: parent
                            onClicked: if (typeof appList !== "undefined" && appList) appList.launch(modelData.id)
                        }
                    }
                }
            }
        }
        Text {
            visible: !(typeof appList !== "undefined" && appList) || appList.apps.length === 0
            anchors.centerIn: parent
            color: root.dim; font.pixelSize: root.unit * 0.035
            text: "No apps found"
        }
    }

    // Bring the phone pane forward when a call starts or arrives.
    Connections {
        target: (typeof phone !== "undefined") ? phone : null
        onCallChanged: if (phone && phone.hasCall) root.activeApp = "phone"
    }

    // ---------------------------------------------------------------- phone
    // Dialer: recent calls, contacts (searchable) and a keypad. Tapping an entry
    // places a call through the phone; its audio rides the car's Bluetooth
    // hands-free. The backend is the "phone" context property (PhoneManager).
    Rectangle {
        id: phoneView
        visible: root.activeApp === "phone"
        x: root.sx; y: root.sy; width: root.sw; height: root.sh
        radius: root.rad
        color: root.surface
        clip: true

        readonly property var ph: (typeof phone !== "undefined") ? phone : null
        property string tab: "recents"          // recents | contacts | keypad
        property string dialBuffer: ""
        readonly property real rowH: Math.round(root.unit * 0.13)
        readonly property real hdrH: Math.round(root.unit * 0.12)

        function fmtDur(s) {
            var m = Math.floor(s / 60), sec = s % 60
            return m + ":" + (sec < 10 ? "0" : "") + sec
        }
        function placeCall(number) { if (ph && number) ph.dial(number) }
        onVisibleChanged: if (visible && ph) ph.refresh()

        // reusable round action button (icon + colour)
        Component {
            id: callButton
            Rectangle {
                property string icon: "phone"
                property color bg: root.accent2
                property color fg: "white"
                property real d: root.unit * 0.12
                signal tapped()
                width: d; height: d; radius: d / 2; color: bg
                Loader {
                    anchors.centerIn: parent; width: parent.d * 0.5; height: width
                    sourceComponent: iconPainter
                    onLoaded: { item.name = parent.icon; item.col = parent.fg }
                }
                MouseArea { anchors.fill: parent; onClicked: parent.tapped() }
            }
        }

        // reusable avatar circle with initials
        Component {
            id: avatar
            Rectangle {
                property string initials: "#"
                property color bg: root.accent
                property real d: phoneView.rowH * 0.66
                // implicit size so a Loader with no explicit size (the call
                // overlay) reserves the right space instead of collapsing to 0.
                implicitWidth: d; implicitHeight: d
                width: d; height: d; radius: d / 2; color: bg
                Text {
                    anchors.centerIn: parent; text: parent.initials; color: "white"
                    font.bold: true; font.pixelSize: parent.d * 0.42
                }
            }
        }

        // ---- header: tab switch ----
        Row {
            id: phoneTabs
            x: root.pad; y: root.pad
            width: parent.width - root.pad * 2; height: phoneView.hdrH - root.pad
            spacing: root.pad
            Repeater {
                model: [ { k: "recents", t: "Recent" },
                         { k: "contacts", t: "Contacts" },
                         { k: "keypad", t: "Keypad" } ]
                Rectangle {
                    width: (phoneTabs.width - root.pad * 2) / 3
                    height: parent.height
                    radius: root.rad
                    color: phoneView.tab === modelData.k ? root.accent : root.surface2
                    Text {
                        anchors.centerIn: parent; text: modelData.t
                        color: phoneView.tab === modelData.k ? "white" : root.fg
                        font.pixelSize: root.unit * 0.036
                        font.bold: phoneView.tab === modelData.k
                    }
                    MouseArea { anchors.fill: parent; onClicked: phoneView.tab = modelData.k }
                }
            }
        }

        // ---- contacts search field (contacts tab) ----
        Rectangle {
            id: contactSearch
            visible: phoneView.tab === "contacts"
            x: root.pad; y: phoneView.hdrH
            width: parent.width - root.pad * 2; height: root.unit * 0.1
            radius: root.rad; color: root.surface2
            TextInput {
                id: contactQuery
                anchors.fill: parent
                anchors.leftMargin: root.pad * 1.5; anchors.rightMargin: root.pad * 1.5
                verticalAlignment: TextInput.AlignVCenter
                color: root.fg; font.pixelSize: root.unit * 0.04
                clip: true
                onTextChanged: if (phoneView.ph) phoneView.ph.setQuery(text)
                Text {
                    anchors.fill: parent; verticalAlignment: Text.AlignVCenter
                    visible: !contactQuery.text.length
                    text: "Search contacts"; color: root.dim; font.pixelSize: root.unit * 0.04
                }
            }
        }

        // ---- list area (recents / contacts) ----
        ListView {
            id: phoneList
            visible: phoneView.tab !== "keypad"
            x: root.pad
            y: phoneView.tab === "contacts" ? phoneView.hdrH + root.unit * 0.1 + root.pad : phoneView.hdrH
            width: parent.width - root.pad * 2
            height: parent.height - y - root.pad
            clip: true
            spacing: 0
            model: !phoneView.ph ? [] : (phoneView.tab === "recents" ? phoneView.ph.recents : phoneView.ph.contacts)

            delegate: Item {
                width: phoneList.width; height: phoneView.rowH
                property bool isRecent: phoneView.tab === "recents"
                property bool missed: isRecent && modelData.missed
                Loader {
                    id: av; anchors.verticalCenter: parent.verticalCenter
                    x: root.pad * 0.5
                    sourceComponent: avatar
                    onLoaded: {
                        item.initials = modelData.initials
                        item.bg = missed ? "#ff4d4f"
                                  : (isRecent && modelData.known === false ? root.surface2 : root.accent)
                    }
                }
                Column {
                    anchors.left: av.right; anchors.leftMargin: root.pad * 1.2
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - av.width - timeLbl.width - root.pad * 4
                    spacing: root.pad * 0.2
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        text: modelData.name
                        color: missed ? "#ff6b6e" : root.fg
                        font.pixelSize: root.unit * 0.044; font.bold: true
                    }
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        color: root.dim; font.pixelSize: root.unit * 0.032
                        text: isRecent
                              ? (modelData.missed ? "Missed call"
                                 : (modelData.incoming ? "Incoming" : "Outgoing")) + " · " + modelData.number
                              : (modelData.label ? modelData.label + " · " : "") + modelData.number
                    }
                }
                Text {
                    id: timeLbl
                    anchors.right: parent.right; anchors.rightMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    visible: isRecent
                    text: isRecent ? modelData.whenText : ""
                    color: root.dim; font.pixelSize: root.unit * 0.032
                }
                Rectangle {
                    anchors.bottom: parent.bottom; width: parent.width; height: 1
                    color: root.surface2
                }
                MouseArea { anchors.fill: parent; onClicked: phoneView.placeCall(modelData.number) }
            }

            // empty state
            Text {
                anchors.centerIn: parent
                visible: phoneList.count === 0
                text: phoneView.tab === "recents" ? "No recent calls" : "No contacts"
                color: root.dim; font.pixelSize: root.unit * 0.04
            }
        }

        // ---- keypad ----
        Item {
            visible: phoneView.tab === "keypad"
            x: root.pad; y: phoneView.hdrH
            width: parent.width - root.pad * 2; height: parent.height - y - root.pad

            // buffer + backspace
            Item {
                id: bufRow
                width: parent.width; height: root.unit * 0.12
                Text {
                    anchors.left: parent.left; anchors.right: bsp.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: phoneView.dialBuffer; color: root.fg
                    font.pixelSize: root.unit * 0.07; elide: Text.ElideLeft
                    horizontalAlignment: Text.AlignHCenter
                }
                Loader {
                    id: bsp; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: root.unit * 0.08; height: width
                    sourceComponent: iconPainter
                    visible: phoneView.dialBuffer.length > 0
                    onLoaded: { item.name = "backspace"; item.col = root.dim }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: phoneView.dialBuffer = phoneView.dialBuffer.slice(0, -1)
                        onPressAndHold: phoneView.dialBuffer = ""
                    }
                }
            }

            Grid {
                id: pad
                anchors.top: bufRow.bottom; anchors.topMargin: root.pad
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 3; rowSpacing: root.pad; columnSpacing: root.pad * 2
                property real key: Math.min((width > 0 ? parent.width : root.unit) * 0.22, root.unit * 0.14)
                Repeater {
                    model: ["1","2","3","4","5","6","7","8","9","*","0","#"]
                    Rectangle {
                        width: pad.key; height: pad.key; radius: width / 2
                        color: keyMa.pressed ? root.accent : root.surface2
                        Text {
                            anchors.centerIn: parent; text: modelData
                            color: keyMa.pressed ? "white" : root.fg
                            font.pixelSize: pad.key * 0.42; font.bold: true
                        }
                        MouseArea {
                            id: keyMa; anchors.fill: parent
                            onClicked: {
                                phoneView.dialBuffer += modelData
                                if (phoneView.ph && phoneView.ph.hasCall) phoneView.ph.sendDtmf(modelData)
                            }
                        }
                    }
                }
            }

            Loader {
                anchors.top: pad.bottom; anchors.topMargin: root.pad * 1.5
                anchors.horizontalCenter: parent.horizontalCenter
                sourceComponent: callButton
                onLoaded: {
                    item.icon = "phone"; item.bg = root.accent2
                    item.d = Qt.binding(function() { return root.unit * 0.14 })
                    item.tapped.connect(function() { phoneView.placeCall(phoneView.dialBuffer) })
                }
            }
        }

        // ---- active-call overlay ----
        Rectangle {
            id: callOverlay
            anchors.fill: parent
            radius: root.rad
            visible: phoneView.ph && phoneView.ph.hasCall
            color: root.surface
            // swallow taps so the list underneath isn't hit
            MouseArea { anchors.fill: parent }

            Column {
                anchors.centerIn: parent
                width: parent.width * 0.8
                spacing: root.pad * 1.2
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: root.unit * 0.22; height: width; radius: width / 2
                    color: root.accent
                    Text {
                        anchors.centerIn: parent; color: "white"
                        font.bold: true; font.pixelSize: parent.width * 0.42
                        text: {
                            var n = phoneView.ph ? (phoneView.ph.callName || phoneView.ph.callNumber) : ""
                            return n ? n.charAt(0).toUpperCase() : "#"
                        }
                    }
                }
                Text {
                    width: parent.width; horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: phoneView.ph ? (phoneView.ph.callName || phoneView.ph.callNumber) : ""
                    color: root.fg; font.bold: true; font.pixelSize: root.unit * 0.06
                }
                Text {
                    width: parent.width; horizontalAlignment: Text.AlignHCenter
                    visible: phoneView.ph && phoneView.ph.callName.length > 0
                    text: phoneView.ph ? phoneView.ph.callNumber : ""
                    color: root.dim; font.pixelSize: root.unit * 0.036
                }
                Text {
                    width: parent.width; horizontalAlignment: Text.AlignHCenter
                    text: !phoneView.ph ? "" : (phoneView.ph.connected
                          ? phoneView.fmtDur(phoneView.ph.callDuration)
                          : phoneView.ph.callStatus)
                    color: root.accent2; font.pixelSize: root.unit * 0.044
                }

                // action buttons
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: root.unit * 0.08
                    topPadding: root.pad
                    // incoming: answer + decline; otherwise mute + hangup
                    Loader {
                        sourceComponent: callButton
                        onLoaded: {
                            var incoming = phoneView.ph && phoneView.ph.incoming
                            item.icon = Qt.binding(function() {
                                return (phoneView.ph && phoneView.ph.incoming) ? "phone"
                                       : (phoneView.ph && phoneView.ph.muted ? "unmute" : "mute")
                            })
                            item.bg = Qt.binding(function() {
                                return (phoneView.ph && phoneView.ph.incoming) ? root.accent2
                                       : (phoneView.ph && phoneView.ph.muted ? root.accent : root.surface2)
                            })
                            item.fg = Qt.binding(function() {
                                return (phoneView.ph && phoneView.ph.incoming) ? "white" : root.fg
                            })
                            item.tapped.connect(function() {
                                if (!phoneView.ph) return
                                if (phoneView.ph.incoming) phoneView.ph.answer()
                                else phoneView.ph.toggleMute()
                            })
                        }
                    }
                    Loader {
                        sourceComponent: callButton
                        onLoaded: {
                            item.icon = "hangup"; item.bg = "#ff4d4f"; item.fg = "white"
                            item.tapped.connect(function() { if (phoneView.ph) phoneView.ph.hangup() })
                        }
                    }
                }
            }
        }
    }

    // -------------------------------------------------------- destination search
    // Full-area overlay: query line, geocode results, and an on-screen keyboard
    // (usable while moving, so a passenger can enter a destination).
    Rectangle {
        id: searchOverlay
        visible: root.searching
        x: root.sx; y: root.sy; width: root.sw; height: root.sh
        radius: root.rad; color: root.surface; clip: true; z: 50

        readonly property real kbH: height * 0.5
        readonly property real queryH: Math.round(root.unit * 0.11)
        // The keyboard stays tucked away while the saved-places list is showing
        // (so the whole list fits); tapping the query line brings it up.
        property bool kbForced: false
        readonly property bool kbShown: !root.chargerMode
                                        && (kbForced || root.searchText.length > 0 || root.pickPlace !== ""
                                            || !root.havePlaces)
        onVisibleChanged: if (!visible) kbForced = false

        // debounce live geocoding as the user types
        Timer {
            id: searchDebounce; interval: 450
            onTriggered: if (root.navMap) root.navMap.search(root.searchText)
        }

        function typeChar(c) { root.searchText += c; searchDebounce.restart() }
        function backspace() { root.searchText = root.searchText.slice(0, -1); searchDebounce.restart() }
        function runSearch() { searchDebounce.stop(); if (root.navMap) root.navMap.search(root.searchText) }

        // --- query line + cancel ---
        Item {
            id: queryRow
            x: root.pad; y: root.pad
            width: parent.width - root.pad * 2; height: searchOverlay.queryH
            Text {
                anchors.left: parent.left; anchors.right: cancelBtn.left; anchors.rightMargin: root.pad
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideLeft
                color: (root.searchText.length || root.chargerMode) ? root.fg : root.dim
                font.pixelSize: parent.height * 0.5
                text: root.chargerMode ? "Charging stations nearby"
                      : root.searchText.length ? root.searchText
                      : root.pickPlace === "home" ? "Search for your home address"
                      : root.pickPlace === "work" ? "Search for your work address"
                      : searchOverlay.kbShown ? "Type a destination" : "Tap to type a destination"
            }
            MouseArea {
                anchors.left: parent.left; anchors.right: cancelBtn.left
                anchors.top: parent.top; anchors.bottom: parent.bottom
                onClicked: if (!root.chargerMode) searchOverlay.kbForced = true
            }
            Rectangle {
                id: cancelBtn
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: parent.height * 1.7; height: parent.height * 0.82; radius: height / 2
                color: root.surface2
                Text { anchors.centerIn: parent; color: root.fg; font.pixelSize: parent.height * 0.4; text: "Cancel" }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        root.searchText = ""
                        if (root.pickPlace) root.pickPlace = ""   // back to the places list
                        else if (root.chargerMode) root.chargerMode = false
                        else root.searching = false
                    }
                }
            }
        }

        // --- saved places (shown until the user types): Home, Work, recents ---
        ListView {
            id: placesList
            visible: root.havePlaces && root.searchText.length < 2 && !root.pickPlace && !root.chargerMode
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: queryRow.bottom; anchors.topMargin: root.pad
            anchors.bottom: searchOverlay.kbShown ? keyboard.top : parent.bottom; anchors.bottomMargin: root.pad
            anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
            clip: true
            model: {
                if (!root.havePlaces) return []
                var pl = carScreen.places, out = []
                out.push({ kind: "home", icon: "home", title: "Home", place: pl.home || null })
                out.push({ kind: "work", icon: "work", title: "Work", place: pl.work || null })
                if (carScreen.evElectric)
                    out.push({ kind: "chargers", icon: "bolt", title: "Charging stations nearby", place: null })
                var rec = carScreen.recentPlaces || []
                for (var i = 0; i < rec.length; i++)
                    out.push({ kind: "recent", icon: "recent", index: i, place: rec[i],
                               title: rec[i].label.split(",").slice(0, 2).join(",") })
                return out
            }
            delegate: Rectangle {
                width: placesList.width; height: root.unit * 0.1
                color: plArea.pressed ? root.surface2 : "transparent"
                readonly property bool saved: modelData.place !== null
                Loader {
                    id: plIcon
                    anchors.left: parent.left; anchors.leftMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.height * 0.5; height: width
                    sourceComponent: iconPainter
                    onLoaded: {
                        item.name = modelData.icon
                        item.col = modelData.kind === "recent" ? root.dim
                                 : modelData.kind === "chargers" ? root.accent2 : root.accent
                    }
                }
                Column {
                    anchors.left: plIcon.right; anchors.leftMargin: root.pad
                    anchors.right: parent.right; anchors.rightMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        color: root.fg; font.pixelSize: root.unit * 0.034; font.bold: modelData.kind !== "recent"
                        text: modelData.title
                    }
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        color: root.dim; font.pixelSize: root.unit * 0.026
                        visible: text.length > 0
                        text: modelData.kind === "recent"
                              ? modelData.place.label.split(",").slice(2, 4).join(",").trim()
                              : modelData.kind === "chargers" ? "From OpenStreetMap, within 25 km"
                              : (saved ? modelData.place.label : "Tap to set")
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.surface2 }
                MouseArea {
                    id: plArea
                    anchors.fill: parent
                    onClicked: {
                        if (modelData.kind === "chargers") root.openChargers()
                        else if (saved) root.goToPlace(modelData.place, modelData.kind === "recent")
                        else root.pickPlace = modelData.kind     // Home/Work not set yet: pick it
                    }
                    // Hold: change Home/Work, or forget a recent destination.
                    onPressAndHold: {
                        if (modelData.kind === "chargers") return
                        if (modelData.kind === "recent") carScreen.removeRecentPlace(modelData.index)
                        else root.pickPlace = modelData.kind
                    }
                }
            }
        }

        // --- results ---
        ListView {
            id: results
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: queryRow.bottom; anchors.topMargin: root.pad
            anchors.bottom: searchOverlay.kbShown ? keyboard.top : parent.bottom; anchors.bottomMargin: root.pad
            anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
            clip: true
            visible: !placesList.visible
            model: !root.navMap ? null : root.chargerMode ? root.navMap.chargerModel : root.navMap.geocodeModel
            delegate: Rectangle {
                width: results.width; height: root.unit * 0.1
                color: resArea.pressed ? root.surface2 : "transparent"
                Column {
                    anchors.left: parent.left; anchors.right: distText.left
                    anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        width: parent.width
                        elide: Text.ElideRight; color: root.fg; font.pixelSize: root.unit * 0.032
                        text: modelData.address.text
                    }
                    // Charger details (power, points, operator).
                    Text {
                        width: parent.width; visible: text.length > 0
                        elide: Text.ElideRight; color: root.dim; font.pixelSize: root.unit * 0.025
                        text: modelData.sub || ""
                    }
                }
                // Distance from here (results are sorted nearest-first).
                Text {
                    id: distText
                    anchors.right: parent.right; anchors.rightMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.dim; font.pixelSize: root.unit * 0.028
                    text: (root.navMap && modelData._dist !== undefined)
                          ? root.navMap._fmtDist(modelData._dist) : ""
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.surface2 }
                MouseArea {
                    id: resArea
                    anchors.fill: parent
                    onClicked: {
                        var c = modelData.coordinate, label = modelData.address.text
                        if (root.pickPlace && root.havePlaces) {
                            // Choosing Home/Work: save it and go back to the places list.
                            carScreen.setPlace(root.pickPlace, label, c.latitude, c.longitude)
                            root.pickPlace = ""; root.searchText = ""
                            return
                        }
                        if (root.navMap) root.navMap.navigateTo(c, label)
                        if (root.havePlaces && !root.chargerMode) carScreen.addRecentPlace(label, c.latitude, c.longitude)
                        root.searching = false; root.searchText = ""
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: results.count === 0 && (root.chargerMode || root.searchText.length >= 2)
                color: root.dim; font.pixelSize: root.unit * 0.032
                text: root.chargerMode
                      ? ((root.navMap && root.navMap.chargerSearching) ? "Looking for chargers…"
                                                                       : "No chargers found within 25 km")
                      : (root.navMap && root.navMap.searching) ? "Searching…" : "No results"
            }
        }

        // --- on-screen QWERTY keyboard ---
        Column {
            id: keyboard
            visible: searchOverlay.kbShown
            anchors.bottom: parent.bottom; anchors.bottomMargin: root.pad
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - root.pad * 2
            height: searchOverlay.kbH
            spacing: root.pad * 0.5
            // 4 character rows (a top number row for house numbers) + the action row.
            property real rowH: (height - spacing * 4) / 5

            Repeater {
                model: [ "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" ]
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: root.pad * 0.5
                    property string letters: modelData
                    Repeater {
                        model: parent.letters.length
                        Rectangle {
                            width: (keyboard.width - root.pad * 0.5 * 9) / 10
                            height: keyboard.rowH; radius: root.rad * 0.5
                            color: keyArea.pressed ? root.accent : root.surface2
                            property string ch: parent.letters.charAt(index)
                            Text {
                                anchors.centerIn: parent
                                color: keyArea.pressed ? "white" : root.fg
                                font.pixelSize: parent.height * 0.42; text: parent.ch
                            }
                            MouseArea { id: keyArea; anchors.fill: parent; onClicked: searchOverlay.typeChar(parent.ch) }
                        }
                    }
                }
            }
            // bottom row: space, backspace, search
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: root.pad * 0.5
                Rectangle {
                    width: keyboard.width * 0.5; height: keyboard.rowH; radius: root.rad * 0.5
                    color: spaceArea.pressed ? root.accent : root.surface2
                    Text { anchors.centerIn: parent; color: root.fg; font.pixelSize: parent.height * 0.34; text: "space" }
                    MouseArea { id: spaceArea; anchors.fill: parent; onClicked: searchOverlay.typeChar(" ") }
                }
                Rectangle {
                    width: keyboard.width * 0.22; height: keyboard.rowH; radius: root.rad * 0.5
                    color: bsArea.pressed ? root.accent : root.surface2
                    Text { anchors.centerIn: parent; color: root.fg; font.pixelSize: parent.height * 0.4; text: "⌫" }
                    MouseArea { id: bsArea; anchors.fill: parent; onClicked: searchOverlay.backspace() }
                }
                Rectangle {
                    width: keyboard.width * 0.27; height: keyboard.rowH; radius: root.rad * 0.5
                    color: root.accent
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: parent.height * 0.36; font.bold: true; text: "Search" }
                    MouseArea { anchors.fill: parent; onClicked: searchOverlay.runSearch() }
                }
            }
        }
    }

    // Full-area play-queue overlay: the current player's track list (from MPRIS
    // TrackList). Tap a row to jump to that song. Opened by tapping the artwork.
    Rectangle {
        id: queueOverlay
        visible: root.showQueue
        x: root.sx; y: root.sy; width: root.sw; height: root.sh
        radius: root.rad; color: root.surface; clip: true; z: 51

        Item {
            id: qHeader
            x: root.pad; y: root.pad
            width: parent.width - root.pad * 2; height: Math.round(root.unit * 0.11)
            Text {
                anchors.left: parent.left; anchors.right: closeQ.left; anchors.rightMargin: root.pad
                anchors.verticalCenter: parent.verticalCenter
                elide: Text.ElideRight; color: root.fg
                font.pixelSize: parent.height * 0.48; font.bold: true
                text: qsTr("Up next") + (root.hasMedia && media.title ? "  ·  " + media.title : "")
            }
            Rectangle {
                id: closeQ
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: parent.height * 1.7; height: parent.height * 0.82; radius: height / 2
                color: root.surface2
                Text { anchors.centerIn: parent; color: root.fg; font.pixelSize: parent.height * 0.4; text: qsTr("Close") }
                MouseArea { anchors.fill: parent; onClicked: root.showQueue = false }
            }
        }

        ListView {
            id: queueList
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: qHeader.bottom; anchors.topMargin: root.pad
            anchors.bottom: parent.bottom; anchors.bottomMargin: root.pad
            anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
            clip: true
            model: root.hasMedia ? media.queue : []
            delegate: Rectangle {
                width: queueList.width; height: root.unit * 0.105
                color: qRow.pressed ? root.surface2 : "transparent"
                Row {
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.pad
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: root.unit * 0.022; height: width; radius: width / 2
                        color: modelData.current ? root.accent : "transparent"
                    }
                    Column {
                        width: parent.width - root.unit * 0.022 - parent.spacing
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            color: modelData.current ? root.accent : root.fg
                            font.pixelSize: root.unit * 0.034; font.bold: modelData.current === true
                            text: modelData.title ? modelData.title : qsTr("Unknown track")
                        }
                        Text {
                            width: parent.width; elide: Text.ElideRight
                            color: root.dim; font.pixelSize: root.unit * 0.026
                            text: modelData.artist ? modelData.artist : ""
                        }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.surface2 }
                MouseArea {
                    id: qRow
                    anchors.fill: parent
                    onClicked: { media.goToTrack(modelData.id); root.showQueue = false }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: queueList.count === 0
                color: root.dim; font.pixelSize: root.unit * 0.032
                horizontalAlignment: Text.AlignHCenter
                width: parent.width - root.pad * 2; wrapMode: Text.Wrap
                text: qsTr("No track list available from this player")
            }
        }
    }

}
