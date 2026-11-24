import QtQuick

// Header band: one card per game at the right. `art` is an empty image the cards' glass samples (no artwork).
Item {
    id: hero
    property alias source: art.source
    property alias art: art
    property var games: []
    readonly property int navZone: 1   // controller / keyboard navigation's band (Main.qml navMove)
    clip: true

    // the picture the game cards' glass samples; not drawn itself (its clipped holder has no height)
    Item {
        width: hero.width
        height: 0
        clip: true
        Image {
            id: art
            width: hero.width
            y: -width * 0.0113   // same crop as the old Tk window (starts about 2 % into the image)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.gutter
        anchors.top: parent.top
        anchors.topMargin: 42
        spacing: 12
        Repeater {
            model: hero.games
            GameCard {
                width: 172
                height: 92
                game: modelData
                backdrop: art
            }
        }
    }
}
