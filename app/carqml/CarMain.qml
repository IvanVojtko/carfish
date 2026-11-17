import QtQuick 2.6

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
    // Preview-only: open the full-screen map state for a still (host preview).
    Component.onCompleted: if (typeof previewExpanded !== "undefined" && previewExpanded === "1")
                               root.mapExpanded = true

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
    readonly property real railT: Math.round((root.landscape ? width : height) * (root.landscape ? 0.15 : 0.13))
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

    // Map dominates (like Android Auto); the now-playing pane is the smaller one.
    readonly property real mapSplit: 0.70
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
    property point lastTouch: Qt.point(-1000, -1000)
    // Tap the map to expand it full-screen (media collapses to a bottom
    // minibar); tap the minibar to return to split view (Android Auto style).
    property bool mapExpanded: false
    readonly property real minibarH: Math.round(unit * 0.17)
    // destination search / navigation (driven by MapPane, id realMap)
    property bool searching: false
    property string searchText: ""
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
            width: Math.min(parent.width - root.pad * 2, root.unit * 0.7)
            height: Math.round(root.unit * 0.12)
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

        // turn card (shown when navigating): next maneuver + distance, from the route
        Rectangle {
            visible: root.navigating
            x: root.pad; y: root.pad
            width: Math.min(parent.width - root.pad * 2, root.unit * 0.7)
            height: root.unit * 0.17
            radius: root.rad * 0.8
            color: root.surface
            Row {
                anchors.fill: parent
                anchors.margins: parent.height * 0.18
                spacing: parent.height * 0.25
                Canvas {
                    id: turnArrow
                    width: parent.height; height: width
                    renderStrategy: Canvas.Immediate; renderTarget: Canvas.Image; antialiasing: true
                    // QtLocation RouteManeuver.Direction: 2-5 right family, 8-11 left,
                    // 6/7 u-turn, 0/1 straight.
                    property int dir: root.navMap ? root.navMap.maneuverDir : 1
                    onDirChanged: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d"); ctx.reset(); var w = width, h = height;
                        ctx.strokeStyle = root.accent2; ctx.fillStyle = root.accent2;
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
                    width: parent.width - parent.height * 1.25 - parent.spacing * 2
                    Text {
                        color: root.fg; font.pixelSize: parent.parent.height * 0.4; font.bold: true
                        text: root.navMap ? root.navMap.nextDistance : ""
                    }
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        color: root.dim; font.pixelSize: parent.parent.height * 0.26
                        text: root.navMap ? (root.navMap.nextInstruction || root.navMap.summary) : ""
                    }
                }
                // clear route
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.height * 0.75; height: width; radius: width / 2
                    color: root.surface2
                    Text { anchors.centerIn: parent; color: root.fg; font.pixelSize: parent.height * 0.5; text: "✕" }
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
                                   : Math.min(parent.width - mediaPane.m * 2, parent.height * 0.42)
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
        }

        // transport controls. A container Item with an EXPLICIT size (so its
        // right/left edges resolve cleanly for anchoring) holding a centred row
        // of buttons. Right-aligned in strip mode, bottom-centred in column mode.
        Item {
            id: controls
            property real btn: mediaPane.uh * (mediaPane.strip ? 0.42 : 0.26)
            property real sp: mediaPane.uh * (mediaPane.strip ? 0.09 : 0.06)
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

        readonly property real kbH: height * 0.42
        readonly property real queryH: Math.round(root.unit * 0.11)

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
                color: root.searchText.length ? root.fg : root.dim
                font.pixelSize: parent.height * 0.5
                text: root.searchText.length ? root.searchText : "Type a destination"
            }
            Rectangle {
                id: cancelBtn
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: parent.height * 1.7; height: parent.height * 0.82; radius: height / 2
                color: root.surface2
                Text { anchors.centerIn: parent; color: root.fg; font.pixelSize: parent.height * 0.4; text: "Cancel" }
                MouseArea { anchors.fill: parent; onClicked: { root.searching = false; root.searchText = "" } }
            }
        }

        // --- results ---
        ListView {
            id: results
            anchors.left: parent.left; anchors.right: parent.right
            anchors.top: queryRow.bottom; anchors.topMargin: root.pad
            anchors.bottom: keyboard.top; anchors.bottomMargin: root.pad
            anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
            clip: true
            model: root.navMap ? root.navMap.geocodeModel : null
            delegate: Rectangle {
                width: results.width; height: root.unit * 0.1
                color: resArea.pressed ? root.surface2 : "transparent"
                Text {
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.leftMargin: root.pad; anchors.rightMargin: root.pad
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight; color: root.fg; font.pixelSize: root.unit * 0.032
                    text: modelData.address.text
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.surface2 }
                MouseArea {
                    id: resArea
                    anchors.fill: parent
                    onClicked: {
                        if (root.navMap) root.navMap.navigateTo(modelData.coordinate, modelData.address.text)
                        root.searching = false; root.searchText = ""
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: results.count === 0 && root.searchText.length >= 2
                color: root.dim; font.pixelSize: root.unit * 0.032
                text: (root.navMap && root.navMap.searching) ? "Searching…" : "No results"
            }
        }

        // --- on-screen QWERTY keyboard ---
        Column {
            id: keyboard
            anchors.bottom: parent.bottom; anchors.bottomMargin: root.pad
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - root.pad * 2
            height: searchOverlay.kbH
            spacing: root.pad * 0.5
            property real rowH: (height - spacing * 3) / 4

            Repeater {
                model: [ "qwertyuiop", "asdfghjkl", "zxcvbnm" ]
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

    // ------------------------------------------------ touch feedback overlay
    // Shows where the head unit tapped, without stealing clicks from the
    // controls below (press is declined so it propagates).
    MouseArea {
        anchors.fill: parent
        z: 1000
        onPressed: {
            root.lastTouch = Qt.point(mouse.x, mouse.y);
            ripple.restart();
            mouse.accepted = false;
        }
    }
    Rectangle {
        id: marker
        visible: ripple.running
        width: root.unit * 0.12; height: width; radius: width / 2
        x: root.lastTouch.x - width / 2
        y: root.lastTouch.y - height / 2
        color: "transparent"
        border.color: root.accent
        border.width: Math.max(2, width * 0.06)
        opacity: 0.9
        NumberAnimation {
            id: ripple; target: marker; property: "opacity"
            from: 0.9; to: 0.0; duration: 600
        }
    }
}
