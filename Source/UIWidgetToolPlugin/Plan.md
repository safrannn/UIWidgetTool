UI Widget Testing Tool

A gameplay/UX shortcut tool for previewing widget blueprints in isolation in Unreal Engine.

User flow

Step 1 — Right-click the widget blueprint asset

In the content browser, right-click a Widget Blueprint (.uasset).
Context menu entry appears: "Add to UI widget tool". Filter it out for abstract/deprecated widget classes.
On select: create a new FWidgetPreview (with a fresh FGuid, a TSoftClassPtr to the widget, and a default LevelBookmark of <widget name>_default) and add it to the manager. Persisted per-project.
No reflected values are captured or stored at this point (see Data model).

Step 2 — Open the manager

Button on the editor menu/tool bar opens (or focuses) a dockable manager window listing every saved entry.

Step 3 — Manager window (table)
Each row shows: widget name, level bookmark (editable, keyed off Id so duplicates are fine), play button, delete button.

Rows whose widget asset was deleted/renamed show a broken/invalid state with a cleanup action.
The reflected property list and each property's CDO value are recomputed live when the window opens — never persisted.

Step 4 — Play

Preview the widget in a dedicated editor tab (retainer / SViewport host or a lightweight preview world) rather than opening/PIE-ing a full level. (Decide preview mode explicitly: editor-only preview vs. PIE — it drives the implementation.)
Resolve WidgetClass fresh, CreateWidget, apply only the user's non-empty overrides, add to the preview.
Hot reload: subscribe to UBlueprint::OnCompiled() (or the global Kismet compile hook). On recompile the class/CDO are reinstanced, so re-resolve the class, destroy the old widget, and rebuild. Provide a manual refresh button.

Step 5 — Delete

Remove the row's entry (by Id) from the saved list.
Value semantics
Empty override → use the property's CDO value. Nothing is applied; CreateWidget already leaves the CDO value in place.
Non-empty override → apply it. Resolve the FProperty by name on the freshly-resolved class each rebuild, ImportText the string into the widget instance. On failure (property removed, type changed) discard that override and flag the row.
Persist only non-empty overrides; drop empty rows on save. This eliminates stale-snapshot problems.
The greyed-out placeholder showing the CDO value is recomputed live (WidgetClass.LoadSynchronous()->GetDefaultObject(), ExportText per property) on manager open and every recompile.
ExportText/ImportText is reliable for primitives; grey out or skip object refs, structs, containers, and instanced subobjects rather than storing garbage.
Optional polish: if a stored override equals the current CDO value, clear it so the row reads as "not overridden."
Data model
cpp
// One user-set override. Only stored when the user actually overrode the property.
USTRUCT()
struct FPreviewInputDescriptor
{
    GENERATED_BODY()
    UPROPERTY() FName   PropertyName;     // reflected property name
    UPROPERTY() FString OverrideValue;    // ImportText-applied; empty rows not persisted
    // CppDataType and the CDO value are live-computed for display, never stored.
};

// One saved widget entry (a row in the manager).
USTRUCT()
struct FWidgetPreview
{
    GENERATED_BODY()
    UPROPERTY() FGuid                        Id;           // stable identity; rows key off this
    UPROPERTY() FString                      LevelBookmark; // editable label
    UPROPERTY() FString                      WidgetName;   // display name
    UPROPERTY() TSoftClassPtr<UUserWidget>   WidgetClass;  // soft ref only
    UPROPERTY() TArray<FPreviewInputDescriptor> Overrides;    // user-set overrides only
};

// Persistent per-project store.
// Consider a per-project UDataAsset or JSON/USaveGame instead of ini —
// nested TArray<struct> round-tripping through EditorPerProjectUserSettings
// is clunky and easy to corrupt. Verify early if you keep this.
UCLASS(config = EditorPerProjectUserSettings)
class UUIPreviewManagerSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(config, EditAnywhere)
    TArray<FWidgetPreview> Entries;
};
Lifetime / cleanup
On tab close: destroy the preview widget and unregister the compile delegate.
Never cache a resolved UClass*/CDO across compiles — reinstancing invalidates them; always re-resolve from the soft ptr.
Open decisions to lock before building
Preview mode: editor-only preview vs. PIE — pick one.
Persistence backend: confirm nested-struct round-tripping in ini, or switch to DataAsset/JSON.
