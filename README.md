# UI Widget Tool

A Unreal Engine plugin to allow preview UI widget in the PIE window.

Play a level in the engine, capture the progress checkpoint together with a widget snapshot, then come back to the marked progress in the level to preview the update UI widget. Review the snapshot nodes in the snapshot viewer, and update UI widget with claude and UE5 MCP.

## Install

#### Prebuilt(for windows + UE5.8): 
1. Download the release file located inside `Release`. Unzip `UIWidgetToolPlugin-<version>-UE5.8-Win64.zip` into `<YourProject>/Plugins/`. `UIWidgetToolPlugin/` folder should appear in the plugin folder.
2. Enable **UI Widget Tool** in Edit > Plugins if it is not already on.

#### Build From source: 
1. Clone the project into `<YourProject>/Plugins/`.
2. Regenerate project files and rebuild.
3. Enable **UI Widget Tool** in Edit > Plugins if it is not already on.

## Usage

### Plugin 
- **Open the manager tab:** either by clicking the toolbar button, or Window > Tools > UI Widget Tool.
- **Save Level Checkpoint:** play a level in the viewport, capture the current progress with Alt+F3(or press Shift+F1 to release the cursor from viewport then click on the capture button). A level checkpoint + a snapshot of the current game view will be saved into the checkpoint path(default at `<YourProject>Saved\UIWidgetTool\Checkpoints`).
- **Create a new row then play:** open the tool's manager window, create a new row in the list then select a widget with the level checkpoint you want come back to. Then click the play button in top left corner of the utility panel on the bottom left to go back to that progress. Then a fresh PIE session will start.
- **Update the wiget with UE5 MCP:** in the utility panel, you can talk with LLM to modify a copy of the original the widget using the MCP in unreal engine. Pick a UI component with `Pick Snapshot Widget` button to let the tool know which one you want to modify. Clicking on the play button will still bring you back to the same checkpoint. 
   - This tool never edits a widget blueprint registered in this project and instead it copies the registered file upon the first time edit. New files are saved inthe `<YourProject>\Saved\UIWidgetTool\WidgetBlueprints`.
- **Snapshot Viewer:** view the `.widgetsnapshot` captured together with the checkpoint. **Runtime / Blueprint** switches between the captured
  slate tree and the blueprint's widget design tree. 

### Capturing checkpoint and snapshot files 
Each capture writes a new `.lvlcp` and `.json` file, by default into `<Project>/Saved/UIWidgetTool/Checkpoints/`. Things that are not captured and restored including: animation state, timers, latent actions, state tree/behavior tree execution, Niagara and audio position, replication, etc.

### Claude connection 
1. Install the [Claude Code CLI](https://docs.claude.com/en/docs/claude-code)(not the claude.exe desktop app) and run `claude` in a terminal. It will ask you to log in just once.
2. Click **Connect MCP** button in the chat panel. It finds the CLI automatically, if not then set `ClaudeExecutable` in Edt > Editor Preferences > Plugins > UI Widget Tool (local).
3. Select a row, type what you want to change, then press **Send**(or Ctrl+Enter).


