import QtQuick
import QtQuick.Dialogs

// 本地文件选择共享组件
Item {
    required property var backend
    FolderDialog {
        id: localFolderDialog
        objectName: "localMusicFolderDialog"
        title: "导入本地音乐文件夹"
        onAccepted: backend.importLocal([selectedFolder])
    }
    FileDialog {
        id: localFileDialog
        objectName: "localMusicFileDialog"
        title: "导入本地音乐文件"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["音乐文件 (*.mp3 *.flac *.m4a *.mp4 *.aac *.ogg *.opus *.ape *.wav *.aiff *.aif *.wv *.wma *.mpc)"]
        onAccepted: backend.importLocal(selectedFiles)
    }
    Connections {
        target: backend
        function onLocalImportRequested(folder: bool) {
            if (folder) {
                localFolderDialog.open();
            } else {
                localFileDialog.open();
            }
        }
    }
}
