# UI Widget Tool
Quick UI widget testing: register and preview each UI widget blueprint in a new isolated level, hot-reload on compile.

## Install
1. Copy the `UIWidgetTool` folder into `<YourProject>/Plugins/`.
2. Regenerate project files and rebuild (Development Editor).
3. Enable **UI Widget Tool** in Edit > Plugins if not enabled by default.

Tested target: UE 5.3+. API notes below if you're on a different version.

## Use
- **Add:** right-click a widget blueprint file in the content browser -> add the widget to tool.
- **Manage:** open the UI widget tool manager from the toolbar button (or Window menu ->
  Tools -> *UI Widget Tool*). Level Bookmark is editable.
- **Play:** opens a preview tab for the selected widget. The widget
  rebuilds automatically when its blueprint recompiles.