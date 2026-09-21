# In-plugin test content

This folder is the answer to the versioning prerequisite that blocked milestone
3: **test maps and test-actor Blueprints for the acceptance runs live in the
plugin repository**, under `Plugins/UIWidgetToolPlugin/Content/`, which the
`.uplugin` already permits via `"CanContainContent": true`.

The alternatives were a submodule or a sibling repository for the project's
`Content/`. Both were rejected for the same reason: milestones 3, 5, 8, and 10 all
need purpose-built fixtures, and a fixture that lives outside the plugin
repository cannot be bisected against a plugin commit. Keeping them here makes
every acceptance run reproducible from a single clone.

The project's own maps - `/Game/ThirdPerson/Lvl_ThirdPerson`,
`/Game/Variant_Combat/Lvl_Combat`, and the other two in the manual acceptance
matrix - stay in the project and are *not* moved here. They are pre-existing
project content, not fixtures, and the manual matrix runs against them as they
are. What belongs here is only content authored for verification:

- test-actor Blueprints implementing `IUIWidgetToolSaveable`;
- any small map built purely to exercise a checklist item, such as the duplicate
  map asset name case, which the project no longer contains and which has to be
  constructed on purpose.

Referencing them from the manager uses the `/UIWidgetToolPlugin/` mount point,
e.g. `/UIWidgetToolPlugin/Fixtures/BP_MyTestActor`.
