#include "UIWTDesignMerge.h"

#include "Core/UIWTWidgetSpec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
  using namespace UIWTDesignTree;
  using namespace UIWTDesignMerge;
  using UIWTWidgetSpec::FClassInfo;

  // ---------------------------------------------------------------------
  // Spec trees, flattened by widget name

  struct FFlat
  {
    FString Class;
    // "" for the root.
    FString Parent;
    TSharedPtr<FJsonObject> Props;
    TSharedPtr<FJsonObject> Slot;
    TOptional<bool> bVariable;
    TArray<FString> Children;
  };

  struct FTree
  {
    FString Root;
    TMap<FString, FFlat> Widgets;
    // Pre-order, so walks over a tree are deterministic.
    TArray<FString> Order;
  };

  FString NameOf(const TSharedPtr<FJsonObject> &InNode)
  {
    FString Name;
    InNode->TryGetStringField(TEXT("name"), Name);
    return Name;
  }

  void Flatten(const TSharedPtr<FJsonObject> &InNode, const FString &InParent, FTree &OutTree)
  {
    FFlat Flat;
    const FString Name = NameOf(InNode);
    InNode->TryGetStringField(TEXT("class"), Flat.Class);
    Flat.Parent = InParent;
    const TSharedPtr<FJsonObject> *Object = nullptr;
    if (InNode->TryGetObjectField(TEXT("props"), Object))
    {
      Flat.Props = *Object;
    }
    if (InNode->TryGetObjectField(TEXT("slot"), Object))
    {
      Flat.Slot = *Object;
    }
    bool bVariable = false;
    if (InNode->TryGetBoolField(TEXT("variable"), bVariable))
    {
      Flat.bVariable = bVariable;
    }
    TArray<TSharedPtr<FJsonObject>> ChildNodes;
    const TArray<TSharedPtr<FJsonValue>> *Children = nullptr;
    if (InNode->TryGetArrayField(TEXT("children"), Children))
    {
      for (const TSharedPtr<FJsonValue> &Child : *Children)
      {
        const TSharedPtr<FJsonObject> ChildObject = Child->AsObject();
        if (ChildObject.IsValid())
        {
          Flat.Children.Add(NameOf(ChildObject));
          ChildNodes.Add(ChildObject);
        }
      }
    }
    OutTree.Order.Add(Name);
    OutTree.Widgets.Add(Name, MoveTemp(Flat));
    for (const TSharedPtr<FJsonObject> &Child : ChildNodes)
    {
      Flatten(Child, Name, OutTree);
    }
  }

  FTree FlattenSpec(const TSharedPtr<FJsonObject> &InSpec)
  {
    FTree Tree;
    const TSharedPtr<FJsonObject> *Root = nullptr;
    if (InSpec.IsValid() && InSpec->TryGetObjectField(TEXT("root"), Root) && Root->IsValid())
    {
      Tree.Root = NameOf(*Root);
      Flatten(*Root, FString(), Tree);
    }
    return Tree;
  }

  // ---------------------------------------------------------------------
  // Values

  // InValue over InDefault: fields InValue leaves out take the default's,
  // since Export leaves out whatever equals the class default, structs'
  // fields included.
  TSharedPtr<FJsonValue> Overlay(const TSharedPtr<FJsonValue> &InDefault,
                                 const TSharedPtr<FJsonValue> &InValue)
  {
    if (!InValue.IsValid())
    {
      return InDefault;
    }
    if (!InDefault.IsValid() || InValue->Type != EJson::Object || InDefault->Type != EJson::Object)
    {
      return InValue;
    }
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->Values = InDefault->AsObject()->Values;
    for (const auto &Pair : InValue->AsObject()->Values)
    {
      const TSharedPtr<FJsonValue> Default = Result->Values.FindRef(Pair.Key);
      Result->Values.Add(Pair.Key, Overlay(Default, Pair.Value));
    }
    return MakeShared<FJsonValueObject>(Result);
  }

  bool JsonEqual(const TSharedPtr<FJsonValue> &InA, const TSharedPtr<FJsonValue> &InB)
  {
    if (!InA.IsValid() || !InB.IsValid())
    {
      return InA.IsValid() == InB.IsValid();
    }
    return FJsonValue::CompareEqual(*InA, *InB);
  }

  TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject> &InObject, const FString &InKey)
  {
    return InObject.IsValid() ? InObject->TryGetField(InKey) : nullptr;
  }

  TArray<FString> Keys(const TSharedPtr<FJsonObject> &InA, const TSharedPtr<FJsonObject> &InB,
                       const TSharedPtr<FJsonObject> &InC = nullptr)
  {
    TSet<FString> Keys;
    for (const TSharedPtr<FJsonObject> &Object : {InA, InB, InC})
    {
      if (Object.IsValid())
      {
        for (const auto &Pair : Object->Values)
        {
          Keys.Add(FString(*Pair.Key));
        }
      }
    }
    TArray<FString> Sorted = Keys.Array();
    Sorted.Sort();
    return Sorted;
  }

  TSharedRef<FJsonObject> Copy(const TSharedPtr<FJsonObject> &InObject)
  {
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    if (InObject.IsValid())
    {
      Result->Values = InObject->Values;
    }
    return Result;
  }

  // ---------------------------------------------------------------------
  // The merge

  enum class ESide : uint8
  {
    Source,
    Blueprint
  };

  // What the merged spec does with one widget.
  struct FDecision
  {
    FString Class;
    FString Parent;
    // Which version's parent this is: decides whose slot values fit.
    ESide ParentFrom = ESide::Source;
    TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Slot;
    TOptional<bool> bVariable;
    // The parent it should have is gone, so it went to an ancestor, where
    // no version's slot values fit.
    bool bReparented = false;
  };

  class FMerger
  {
  public:
    FMerger(const FInput &InInput, FClassInfo &InClasses) : In(InInput), Classes(InClasses) {}

    FResult Run();

  private:
    const FInput &In;
    FClassInfo &Classes;
    FResult Result;
    FTree S;
    FTree B;
    TMap<FString, FString> NodeOfWidget;
    TMap<FString, FDecision> Kept;
    TArray<FString> KeptOrder;
    TMap<FString, TArray<FString>> SourceChangeCache;

    UClass *ClassOf(const FString &InClass)
    {
      return InClass.IsEmpty() ? nullptr : Classes.ResolveClass(InClass);
    }

    bool SameClass(const FString &InA, const FString &InB)
    {
      if (InA == InB)
      {
        return true;
      }
      const UClass *A = ClassOf(InA);
      return A && A == ClassOf(InB);
    }

    UClass *SlotClassFor(const FString &InParentClass)
    {
      UClass *Class = ClassOf(InParentClass);
      return Class ? Classes.SlotClass(Class) : nullptr;
    }

    FString ParentClassIn(const FTree &InTree, const FString &InName) const
    {
      const FFlat *Widget = InTree.Widgets.Find(InName);
      const FFlat *Parent = Widget ? InTree.Widgets.Find(Widget->Parent) : nullptr;
      return Parent ? Parent->Class : FString();
    }

    FString BaseParentClass(const FString &InName) const
    {
      const FBaseWidget *Widget = In.BaseWidgets.Find(InName);
      const FBaseWidget *Parent = Widget ? In.BaseWidgets.Find(Widget->Parent) : nullptr;
      return Parent ? Parent->Class : FString();
    }

    // Two versions of one property compared as effective values: what's
    // left out is the class default, and numbers are normalized as Export
    // writes them, so float noise never counts as a change.
    bool Same(UClass *InClass, const FString &InKey, const TSharedPtr<FJsonValue> &InA,
              const TSharedPtr<FJsonValue> &InB)
    {
      if (!InA.IsValid() && !InB.IsValid())
      {
        return true;
      }
      const TSharedPtr<FJsonValue> Default = InClass ? Classes.Default(InClass, InKey) : nullptr;
      return JsonEqual(UIWTWidgetSpec::NormalizeValue(Overlay(Default, InA)),
                       UIWTWidgetSpec::NormalizeValue(Overlay(Default, InB)));
    }

    TArray<FString> Differences(UClass *InClass, const TSharedPtr<FJsonObject> &InA,
                                const TSharedPtr<FJsonObject> &InB)
    {
      TArray<FString> Found;
      for (const FString &Key : Keys(InA, InB))
      {
        if (!Same(InClass, Key, Field(InA, Key), Field(InB, Key)))
        {
          Found.Add(Key);
        }
      }
      return Found;
    }

    TArray<FString> SourceChanges(const FString &InName);
    void Note(const FString &InWidget, const TCHAR *InCategory, const FString &InDetail);
    void Preview();
    void Decide(const FString &InName);
    void Keep(const FString &InName, FDecision &&InDecision);
    TSharedRef<FJsonObject> MergeValues(UClass *InClass, const TSharedPtr<FJsonObject> &InBase,
                                        const TSharedPtr<FJsonObject> *InExported,
                                        const TSharedPtr<FJsonObject> &InBlueprint,
                                        const TSharedPtr<FJsonObject> &InSource, bool bInSlot,
                                        bool bInOnlyClassProperties, TArray<FString> &OutConflicts,
                                        TArray<FString> &OutKept, TArray<FString> &OutDropped);
    FString Ancestor(const FString &InName, ESide InSide) const;
    bool BuildTree();
    TArray<FString> OrderChildren(const FString &InParent, const TArray<FString> &InChildren) const;
    void MergeSlot(const FString &InName);
    TSharedRef<FJsonObject> Emit(const FString &InName,
                                 const TMap<FString, TArray<FString>> &InChildren, bool bInRoot);
  };

  // What the design changed in one widget since the last import, one entry
  // per kind of change; empty when nothing did.
  TArray<FString> FMerger::SourceChanges(const FString &InName)
  {
    if (const TArray<FString> *Cached = SourceChangeCache.Find(InName))
    {
      return *Cached;
    }
    TArray<FString> Details;
    const FFlat *Source = S.Widgets.Find(InName);
    const FBaseWidget *Base = In.BaseWidgets.Find(InName);
    if (Source && Base)
    {
      if (!SameClass(Source->Class, Base->Class))
      {
        Details.Add(FString::Printf(TEXT("class %s → %s"), *Base->Class, *Source->Class));
      }
      else
      {
        const TArray<FString> Props = Differences(ClassOf(Source->Class), Base->Props, Source->Props);
        if (!Props.IsEmpty())
        {
          Details.Add(TEXT("props ") + FString::Join(Props, TEXT(", ")));
        }
      }
      if (Source->Parent != Base->Parent)
      {
        Details.Add(FString::Printf(TEXT("parent %s → %s"), *Base->Parent, *Source->Parent));
      }
      else
      {
        const TArray<FString> Slot =
            Differences(SlotClassFor(ParentClassIn(S, InName)), Base->Slot, Source->Slot);
        if (!Slot.IsEmpty())
        {
          Details.Add(TEXT("slot ") + FString::Join(Slot, TEXT(", ")));
        }
      }
    }
    SourceChangeCache.Add(InName, Details);
    return Details;
  }

  void FMerger::Note(const FString &InWidget, const TCHAR *InCategory, const FString &InDetail)
  {
    const FString *Node = NodeOfWidget.Find(InWidget);
    Result.Report.Add({Node ? *Node : FString(), InCategory, InWidget + TEXT(": ") + InDetail});
    if (FCString::Strcmp(InCategory, TEXT("conflict")) == 0)
    {
      ++Result.Conflicts;
    }
    else if (FCString::Strcmp(InCategory, TEXT("keptUEChange")) == 0)
    {
      ++Result.KeptUEChanges;
    }
  }

  // The preview: every node the design added, removed or changed since the
  // last import, compared with the sidecar alone.
  void FMerger::Preview()
  {
    TSet<FString> NodeSet;
    for (const TPair<FString, FRoleNames> &Pair : In.BaseOwned)
    {
      NodeSet.Add(Pair.Key);
    }
    for (const TPair<FString, FRoleNames> &Pair : In.SourceOwned)
    {
      NodeSet.Add(Pair.Key);
    }
    TArray<FString> Nodes = NodeSet.Array();
    Nodes.Sort();
    for (const FString &Node : Nodes)
    {
      const FRoleNames *Base = In.BaseOwned.Find(Node);
      const FRoleNames *Source = In.SourceOwned.Find(Node);
      FChange Change;
      Change.Node = Node;
      if (!Base || !Source)
      {
        Change.Change = Base ? EChange::Removed : EChange::Added;
        TArray<FString> Roles;
        (Base ? *Base : *Source).GetKeys(Roles);
        Roles.Sort();
        for (const FString &Role : Roles)
        {
          Change.Details.Add(FString::Printf(TEXT("%s (%s)"), *(Base ? *Base : *Source)[Role], *Role));
        }
        Result.Changes.Add(MoveTemp(Change));
        continue;
      }
      TSet<FString> RoleSet;
      for (const TPair<FString, FString> &Pair : *Base)
      {
        RoleSet.Add(Pair.Key);
      }
      for (const TPair<FString, FString> &Pair : *Source)
      {
        RoleSet.Add(Pair.Key);
      }
      TArray<FString> Roles = RoleSet.Array();
      Roles.Sort();
      for (const FString &Role : Roles)
      {
        const FString *BaseName = Base->Find(Role);
        const FString *SourceName = Source->Find(Role);
        if (!BaseName)
        {
          Change.Details.Add(FString::Printf(TEXT("%s: new %s widget"), **SourceName, *Role));
        }
        else if (!SourceName)
        {
          Change.Details.Add(FString::Printf(TEXT("%s: %s widget removed"), **BaseName, *Role));
        }
        else if (!BaseName->Equals(*SourceName, ESearchCase::CaseSensitive))
        {
          Change.Details.Add(FString::Printf(TEXT("%s renamed to %s"), **BaseName, **SourceName));
        }
        else if (!SourceChanges(*SourceName).IsEmpty())
        {
          Change.Details.Add(*SourceName + TEXT(": ") +
                             FString::Join(SourceChanges(*SourceName), TEXT("; ")));
        }
      }
      if (!Change.Details.IsEmpty())
      {
        Result.Changes.Add(MoveTemp(Change));
      }
    }
  }

  void FMerger::Keep(const FString &InName, FDecision &&InDecision)
  {
    if (!Kept.Contains(InName))
    {
      KeptOrder.Add(InName);
    }
    Kept.Add(InName, MoveTemp(InDecision));
  }

  // One property object merged value by value: the design's side compares
  // the new conversion with the base's converter values, the blueprint's
  // side compares Export with the base's exported values (InExported; null
  // for an old sidecar, when the converter values stand in). Props of a
  // widget that stays the same object may leave a value out (Apply keeps
  // it); slots are built again, so every value a slot keeps is written.
  TSharedRef<FJsonObject> FMerger::MergeValues(UClass *InClass, const TSharedPtr<FJsonObject> &InBase,
                                               const TSharedPtr<FJsonObject> *InExported,
                                               const TSharedPtr<FJsonObject> &InBlueprint,
                                               const TSharedPtr<FJsonObject> &InSource,
                                               bool bInSlot, bool bInOnlyClassProperties,
                                               TArray<FString> &OutConflicts,
                                               TArray<FString> &OutKept,
                                               TArray<FString> &OutDropped)
  {
    TSharedRef<FJsonObject> Merged = MakeShared<FJsonObject>();
    TArray<FString> AllKeys = Keys(InBase, InBlueprint, InSource);
    if (InExported)
    {
      for (const FString &Key : Keys(*InExported, nullptr))
      {
        AllKeys.AddUnique(Key);
      }
    }
    for (const FString &Key : AllKeys)
    {
      const TSharedPtr<FJsonValue> BaseValue = Field(InBase, Key);
      const TSharedPtr<FJsonValue> BlueprintValue = Field(InBlueprint, Key);
      const TSharedPtr<FJsonValue> SourceValue = Field(InSource, Key);
      if (bInOnlyClassProperties && !Classes.HasProperty(InClass, Key))
      {
        if (SourceValue.IsValid())
        {
          OutDropped.Add(Key);
        }
        continue;
      }
      const bool bSourceChanged = !Same(InClass, Key, BaseValue, SourceValue);
      // Export against Export: exact, derived values included. Without a
      // snapshot, properties Export never writes count as unchanged.
      const bool bUEChanged =
          InExported ? !JsonEqual(UIWTWidgetSpec::NormalizeValue(Field(*InExported, Key)),
                                  UIWTWidgetSpec::NormalizeValue(BlueprintValue))
                     : Classes.IsExported(InClass, Key) &&
                           !Same(InClass, Key, BaseValue, BlueprintValue);
      if (!bSourceChanged)
      {
        if (bUEChanged)
        {
          OutKept.Add(Key);
          if (bInSlot && BlueprintValue.IsValid())
          {
            Merged->SetField(Key, BlueprintValue);
          }
        }
        else if (SourceValue.IsValid())
        {
          Merged->SetField(Key, SourceValue);
        }
        continue;
      }
      if (bUEChanged && !Same(InClass, Key, SourceValue, BlueprintValue))
      {
        OutConflicts.Add(Key);
      }
      if (SourceValue.IsValid())
      {
        Merged->SetField(Key, SourceValue);
      }
      else if (!bInSlot && BlueprintValue.IsValid())
      {
        // The design went back to the class default and the widget didn't:
        // a left-out prop would keep the widget's value.
        if (const TSharedPtr<FJsonValue> Default = Classes.Default(InClass, Key))
        {
          Merged->SetField(Key, Default);
        }
      }
    }
    return Merged;
  }

  void FMerger::Decide(const FString &InName)
  {
    const FFlat *Source = S.Widgets.Find(InName);
    const FFlat *Blueprint = B.Widgets.Find(InName);
    const FBaseWidget *Base = In.BaseWidgets.Find(InName);
    const TArray<FString> *Reasons = In.Protected.Find(InName);
    const FString ReasonText = Reasons ? FString::Join(*Reasons, TEXT(", ")) : FString();

    // Added in UE (or by a Claude pass): kept as it is, where it is.
    if (Blueprint && !Base && !Source)
    {
      FDecision Decision;
      Decision.Class = Blueprint->Class;
      Decision.Parent = Blueprint->Parent;
      Decision.ParentFrom = ESide::Blueprint;
      Decision.bVariable = Blueprint->bVariable;
      Keep(InName, MoveTemp(Decision));
      ++Result.UEWidgets;
      return;
    }

    auto FromSource = [&]()
    {
      FDecision Decision;
      Decision.Class = Source->Class;
      Decision.Parent = Source->Parent;
      Decision.Props = Copy(Source->Props);
      Decision.bVariable = Blueprint ? Blueprint->bVariable : Source->bVariable;
      Keep(InName, MoveTemp(Decision));
    };

    // New in the design.
    if (Source && !Base)
    {
      FromSource();
      return;
    }

    // Deleted in UE: stays deleted unless the design changed it since.
    if (Source && Base && !Blueprint)
    {
      const TArray<FString> Changes = SourceChanges(InName);
      if (Changes.IsEmpty())
      {
        Note(InName, TEXT("keptUEChange"),
             TEXT("deleted in the blueprint; the design didn't change it, so it stays deleted"));
        return;
      }
      Note(InName, TEXT("conflict"),
           FString::Printf(TEXT("deleted in the blueprint, but the design changed it (%s); "
                                "added back"),
                           *FString::Join(Changes, TEXT("; "))));
      FromSource();
      return;
    }

    // Removed from the design: removed, unless something depends on it.
    if (!Source && Base && Blueprint)
    {
      if (Reasons)
      {
        FDecision Decision;
        Decision.Class = Blueprint->Class;
        Decision.Parent = Blueprint->Parent;
        Decision.ParentFrom = ESide::Blueprint;
        Decision.bVariable = Blueprint->bVariable;
        Keep(InName, MoveTemp(Decision));
        Note(InName, TEXT("protectedKept"),
             FString::Printf(TEXT("the design no longer has this layer; kept because of %s"),
                             *ReasonText));
      }
      return;
    }
    if (!Source || !Base || !Blueprint)
    {
      return;
    }

    // In all three versions.
    const TArray<FString> Changes = SourceChanges(InName);
    const bool bSourceChanged = !Changes.IsEmpty();
    FDecision Decision;
    Decision.bVariable = Blueprint->bVariable;

    const bool bUEClass = !SameClass(Blueprint->Class, Base->Class);
    if (Reasons)
    {
      Decision.Class = Blueprint->Class;
    }
    else if (bUEClass && !bSourceChanged)
    {
      Decision.Class = Blueprint->Class;
      Result.KeptFromBlueprint.FindOrAdd(InName).bWhole = true;
      Note(InName, TEXT("keptUEChange"),
           FString::Printf(TEXT("kept the blueprint's class %s"), *Blueprint->Class));
    }
    else if (bUEClass && !SameClass(Source->Class, Blueprint->Class))
    {
      Decision.Class = Source->Class;
      Note(InName, TEXT("conflict"),
           FString::Printf(TEXT("the blueprint changed the class to %s and the design changed "
                                "this widget (%s); the design's class %s is used"),
                           *Blueprint->Class, *FString::Join(Changes, TEXT("; ")),
                           *Source->Class));
    }
    else
    {
      Decision.Class = Source->Class;
    }

    const bool bUEParent = Blueprint->Parent != Base->Parent;
    if (bUEParent && !bSourceChanged)
    {
      Decision.Parent = Blueprint->Parent;
      Decision.ParentFrom = ESide::Blueprint;
      Note(InName, TEXT("keptUEChange"),
           FString::Printf(TEXT("kept where the blueprint moved it (into %s)"),
                           *Blueprint->Parent));
    }
    else if (bUEParent && Source->Parent != Blueprint->Parent)
    {
      Decision.Parent = Source->Parent;
      Note(InName, TEXT("conflict"),
           FString::Printf(TEXT("the blueprint moved it into %s and the design changed it "
                                "(%s); the design's place (in %s) is used"),
                           *Blueprint->Parent, *FString::Join(Changes, TEXT("; ")),
                           *Source->Parent));
    }
    else
    {
      Decision.Parent = Source->Parent;
    }

    UClass *Class = ClassOf(Decision.Class);
    TArray<FString> Conflicts;
    TArray<FString> KeptKeys;
    TArray<FString> Dropped;
    const TSharedPtr<FJsonObject> *Exported =
        Base->Exported.IsSet() ? &Base->Exported->Props : nullptr;
    const bool bSameObject = SameClass(Decision.Class, Blueprint->Class);
    if (bSameObject && SameClass(Decision.Class, Source->Class))
    {
      Decision.Props = MergeValues(Class, Base->Props, Exported, Blueprint->Props, Source->Props,
                                   false, false, Conflicts, KeptKeys, Dropped);
    }
    else if (bSameObject && Reasons)
    {
      // A protected widget keeps its class; the design's values that class
      // has still apply.
      Decision.Props = MergeValues(Class, Base->Props, Exported, Blueprint->Props, Source->Props,
                                   false, true, Conflicts, KeptKeys, Dropped);
    }
    if (!KeptKeys.IsEmpty())
    {
      Result.KeptFromBlueprint.FindOrAdd(InName).Props.Append(KeptKeys);
    }
    else if (!bSameObject)
    {
      // A new object of the design's class: nothing of the old one remains.
      Decision.Props = Copy(Source->Props);
    }
    if (Reasons && !SameClass(Source->Class, Blueprint->Class))
    {
      Note(InName, TEXT("protectedClass"),
           FString::Printf(TEXT("kept class %s because of %s; the design's class %s wasn't "
                                "applied%s"),
                           *Blueprint->Class, *ReasonText, *Source->Class,
                           Dropped.IsEmpty()
                               ? TEXT("")
                               : *(TEXT(", nor its props ") + FString::Join(Dropped, TEXT(", ")))));
    }
    if (!Conflicts.IsEmpty())
    {
      Note(InName, TEXT("conflict"),
           FString::Printf(TEXT("props %s changed in both the design and the blueprint; the "
                                "design's values are used"),
                           *FString::Join(Conflicts, TEXT(", "))));
    }
    if (!KeptKeys.IsEmpty())
    {
      Note(InName, TEXT("keptUEChange"),
           FString::Printf(TEXT("kept the blueprint's props %s"),
                           *FString::Join(KeptKeys, TEXT(", "))));
    }
    Keep(InName, MoveTemp(Decision));
  }

  // The nearest ancestor of InName in the version its placement came from.
  FString FMerger::Ancestor(const FString &InName, ESide InSide) const
  {
    const FTree &First = InSide == ESide::Blueprint ? B : S;
    const FTree &Second = InSide == ESide::Blueprint ? S : B;
    if (const FFlat *Widget = First.Widgets.Find(InName))
    {
      return Widget->Parent;
    }
    if (const FFlat *Widget = Second.Widgets.Find(InName))
    {
      return Widget->Parent;
    }
    if (const FBaseWidget *Widget = In.BaseWidgets.Find(InName))
    {
      return Widget->Parent;
    }
    return FString();
  }

  // Children in the design's order, with the ones only the blueprint places
  // there after the sibling they follow in the blueprint.
  TArray<FString> FMerger::OrderChildren(const FString &InParent,
                                         const TArray<FString> &InChildren) const
  {
    TArray<FString> Ordered;
    if (const FFlat *Parent = S.Widgets.Find(InParent))
    {
      for (const FString &Child : Parent->Children)
      {
        if (InChildren.Contains(Child))
        {
          Ordered.AddUnique(Child);
        }
      }
    }
    if (const FFlat *Parent = B.Widgets.Find(InParent))
    {
      int32 Last = INDEX_NONE;
      for (const FString &Child : Parent->Children)
      {
        const int32 Index = Ordered.Find(Child);
        if (Index != INDEX_NONE)
        {
          Last = Index;
        }
        else if (InChildren.Contains(Child))
        {
          Ordered.Insert(Child, ++Last);
        }
      }
    }
    for (const FString &Child : InChildren)
    {
      Ordered.AddUnique(Child);
    }
    return Ordered;
  }

  void FMerger::MergeSlot(const FString &InName)
  {
    FDecision &Decision = Kept[InName];
    if (Decision.bReparented)
    {
      Decision.Slot = nullptr;
      return;
    }
    UClass *SlotClass = SlotClassFor(Kept[Decision.Parent].Class);
    const FFlat *Source = S.Widgets.Find(InName);
    const FFlat *Blueprint = B.Widgets.Find(InName);
    const FBaseWidget *Base = In.BaseWidgets.Find(InName);
    auto Fits = [&](const FString &InParent, const FString &InParentClass)
    { return InParent == Decision.Parent && SlotClassFor(InParentClass) == SlotClass; };
    const bool bSourceFits = Source && Fits(Source->Parent, ParentClassIn(S, InName));
    const bool bBlueprintFits = Blueprint && Fits(Blueprint->Parent, ParentClassIn(B, InName));
    const bool bBaseFits = Base && Fits(Base->Parent, BaseParentClass(InName));
    if (bSourceFits && bBlueprintFits && bBaseFits)
    {
      TArray<FString> Conflicts;
      TArray<FString> KeptKeys;
      TArray<FString> Dropped;
      Decision.Slot = MergeValues(SlotClass, Base->Slot,
                                  Base->Exported.IsSet() ? &Base->Exported->Slot : nullptr,
                                  Blueprint->Slot, Source->Slot, true, false, Conflicts, KeptKeys,
                                  Dropped);
      if (!KeptKeys.IsEmpty())
      {
        Result.KeptFromBlueprint.FindOrAdd(InName).Slot.Append(KeptKeys);
      }
      if (!Conflicts.IsEmpty())
      {
        Note(InName, TEXT("conflict"),
             FString::Printf(TEXT("slot %s changed in both the design and the blueprint; the "
                                  "design's values are used"),
                             *FString::Join(Conflicts, TEXT(", "))));
      }
      if (!KeptKeys.IsEmpty())
      {
        Note(InName, TEXT("keptUEChange"),
             FString::Printf(TEXT("kept the blueprint's slot %s"),
                             *FString::Join(KeptKeys, TEXT(", "))));
      }
    }
    else if (Decision.ParentFrom == ESide::Blueprint && bBlueprintFits)
    {
      Decision.Slot = Blueprint->Slot;
      if (Base)
      {
        Result.KeptFromBlueprint.FindOrAdd(InName).bSlotWhole = true;
      }
    }
    else if (bSourceFits)
    {
      Decision.Slot = Source->Slot;
    }
    else if (bBlueprintFits)
    {
      Decision.Slot = Blueprint->Slot;
    }
    else
    {
      Decision.Slot = nullptr;
    }
  }

  TSharedRef<FJsonObject> FMerger::Emit(const FString &InName,
                                        const TMap<FString, TArray<FString>> &InChildren,
                                        bool bInRoot)
  {
    const FDecision &Decision = Kept[InName];
    TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("class"), Decision.Class);
    Node->SetStringField(TEXT("name"), InName);
    if (Decision.bVariable.IsSet())
    {
      Node->SetBoolField(TEXT("variable"), *Decision.bVariable);
    }
    if (!Decision.Props->Values.IsEmpty())
    {
      Node->SetObjectField(TEXT("props"), Decision.Props);
    }
    if (!bInRoot && Decision.Slot.IsValid() && !Decision.Slot->Values.IsEmpty())
    {
      Node->SetObjectField(TEXT("slot"), Decision.Slot);
    }
    if (const TArray<FString> *Children = InChildren.Find(InName))
    {
      TArray<TSharedPtr<FJsonValue>> ChildNodes;
      for (const FString &Child : *Children)
      {
        ChildNodes.Add(MakeShared<FJsonValueObject>(Emit(Child, InChildren, false)));
      }
      Node->SetArrayField(TEXT("children"), ChildNodes);
    }
    return Node;
  }

  bool FMerger::BuildTree()
  {
    // Parents that are gone: the nearest ancestor still here.
    for (const FString &Name : KeptOrder)
    {
      FDecision &Decision = Kept[Name];
      FString Parent = Decision.Parent;
      for (int32 Guard = 0; !Parent.IsEmpty() && !Kept.Contains(Parent) && Guard < 256; ++Guard)
      {
        Parent = Ancestor(Parent, Decision.ParentFrom);
      }
      if (!Kept.Contains(Parent))
      {
        Parent.Reset();
      }
      if (Parent != Decision.Parent)
      {
        Note(Name, TEXT("mergeMoved"),
             FString::Printf(TEXT("its parent %s is gone; moved into %s"), *Decision.Parent,
                             Parent.IsEmpty() ? TEXT("the root") : *Parent));
        Decision.Parent = Parent;
        Decision.bReparented = true;
      }
    }

    // A parent taken from one version and another from the other can form a
    // loop; the widget where it closes goes to the top.
    for (const FString &Name : KeptOrder)
    {
      TSet<FString> Seen;
      FString Walk = Name;
      while (!Walk.IsEmpty() && !Seen.Contains(Walk))
      {
        Seen.Add(Walk);
        Walk = Kept[Walk].Parent;
      }
      if (!Walk.IsEmpty())
      {
        Note(Walk, TEXT("conflict"),
             TEXT("the design's and the blueprint's placements contradict each other; moved "
                  "to the root"));
        Kept[Walk].Parent.Reset();
        Kept[Walk].bReparented = true;
      }
    }

    // One root: the blueprint's if it is still here (it may be a widget
    // added around the design's root), else the design's.
    TArray<FString> Roots;
    for (const FString &Name : KeptOrder)
    {
      if (Kept[Name].Parent.IsEmpty())
      {
        Roots.Add(Name);
      }
    }
    if (Roots.IsEmpty())
    {
      return false;
    }
    const FString Root = Roots.Contains(B.Root) ? B.Root
                         : Roots.Contains(S.Root) ? S.Root
                                                  : Roots[0];
    for (const FString &Name : Roots)
    {
      if (Name != Root)
      {
        Note(Name, TEXT("mergeMoved"), FString::Printf(TEXT("moved into the root %s"), *Root));
        Kept[Name].Parent = Root;
        Kept[Name].bReparented = true;
      }
    }

    // Panels that can't hold what they got (a class kept or changed under
    // them): the extra children go to the nearest panel that can.
    auto ChildrenMap = [&]()
    {
      TMap<FString, TArray<FString>> Map;
      for (const FString &Name : KeptOrder)
      {
        if (Name != Root && Kept.Contains(Name))
        {
          Map.FindOrAdd(Kept[Name].Parent).Add(Name);
        }
      }
      return Map;
    };
    TMap<FString, TArray<FString>> Children = ChildrenMap();
    bool bMoved = false;
    for (const TPair<FString, TArray<FString>> &Pair : Children)
    {
      const UClass *Class = ClassOf(Kept[Pair.Key].Class);
      const int32 Capacity = !Classes.IsPanel(Class)                  ? 0
                             : Classes.CanHaveMultipleChildren(Class) ? MAX_int32
                                                                      : 1;
      if (Pair.Value.Num() <= Capacity)
      {
        continue;
      }
      FString Target = Kept[Pair.Key].Parent;
      while (!Target.IsEmpty() && !Classes.CanHaveMultipleChildren(ClassOf(Kept[Target].Class)))
      {
        Target = Kept[Target].Parent;
      }
      if (Target.IsEmpty() && Classes.CanHaveMultipleChildren(ClassOf(Kept[Root].Class)))
      {
        Target = Root;
      }
      const TArray<FString> Ordered = OrderChildren(Pair.Key, Pair.Value);
      for (int32 Index = Capacity; Index < Ordered.Num(); ++Index)
      {
        const FString &Child = Ordered[Index];
        if (Target.IsEmpty())
        {
          Note(Child, TEXT("mergeDropped"),
               FString::Printf(TEXT("%s can't hold it and no panel above can; left out"),
                               *Pair.Key));
          Kept.Remove(Child);
          continue;
        }
        Note(Child, TEXT("mergeMoved"),
             FString::Printf(TEXT("%s (%s) can't hold it; moved into %s"), *Pair.Key,
                             *Kept[Pair.Key].Class, *Target));
        Kept[Child].Parent = Target;
        Kept[Child].bReparented = true;
      }
      bMoved = true;
    }
    if (bMoved)
    {
      KeptOrder.RemoveAll([this](const FString &Name) { return !Kept.Contains(Name); });
      Children = ChildrenMap();
    }
    for (TPair<FString, TArray<FString>> &Pair : Children)
    {
      Pair.Value = OrderChildren(Pair.Key, Pair.Value);
    }
    for (const FString &Name : KeptOrder)
    {
      if (Name != Root)
      {
        MergeSlot(Name);
      }
    }
    TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
    Spec->SetObjectField(TEXT("root"), Emit(Root, Children, true));
    Result.Spec = Spec;
    return true;
  }

  FResult FMerger::Run()
  {
    S = FlattenSpec(In.Source);
    B = FlattenSpec(In.Blueprint);
    for (const FOwnedMap *Owned : {&In.SourceOwned, &In.BaseOwned})
    {
      for (const TPair<FString, FRoleNames> &Node : *Owned)
      {
        for (const TPair<FString, FString> &Role : Node.Value)
        {
          if (!NodeOfWidget.Contains(Role.Value))
          {
            NodeOfWidget.Add(Role.Value, Node.Key);
          }
        }
      }
    }

    Preview();

    TSet<FString> Seen;
    for (const TArray<FString> *Order : {&S.Order, &B.Order})
    {
      for (const FString &Name : *Order)
      {
        if (!Seen.Contains(Name))
        {
          Seen.Add(Name);
          Decide(Name);
        }
      }
    }
    TArray<FString> BaseOnly;
    In.BaseWidgets.GetKeys(BaseOnly);
    BaseOnly.Sort();
    for (const FString &Name : BaseOnly)
    {
      if (!Seen.Contains(Name))
      {
        Seen.Add(Name);
        Decide(Name);
      }
    }
    BuildTree();
    return MoveTemp(Result);
  }
}

UIWTDesignMerge::FResult UIWTDesignMerge::Merge(const FInput &InInput, FClassInfo &InClasses)
{
  return FMerger(InInput, InClasses).Run();
}

void UIWTDesignMerge::ReadBaseWidgets(const TSharedPtr<FJsonObject> &InWidgets,
                                      TMap<FString, FBaseWidget> &OutWidgets)
{
  if (!InWidgets.IsValid())
  {
    return;
  }
  for (const auto &Pair : InWidgets->Values)
  {
    const TSharedPtr<FJsonObject> Entry = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
    if (!Entry.IsValid())
    {
      continue;
    }
    FBaseWidget Widget;
    Entry->TryGetStringField(TEXT("class"), Widget.Class);
    Entry->TryGetStringField(TEXT("parent"), Widget.Parent);
    const TSharedPtr<FJsonObject> *Object = nullptr;
    if (Entry->TryGetObjectField(TEXT("props"), Object))
    {
      Widget.Props = *Object;
    }
    if (Entry->TryGetObjectField(TEXT("slot"), Object))
    {
      Widget.Slot = *Object;
    }
    if (Entry->TryGetObjectField(TEXT("exported"), Object))
    {
      FExported Exported;
      const TSharedPtr<FJsonObject> *Values = nullptr;
      if ((*Object)->TryGetObjectField(TEXT("props"), Values))
      {
        Exported.Props = *Values;
      }
      if ((*Object)->TryGetObjectField(TEXT("slot"), Values))
      {
        Exported.Slot = *Values;
      }
      Widget.Exported = MoveTemp(Exported);
    }
    OutWidgets.Add(FString(*Pair.Key), MoveTemp(Widget));
  }
}

TMap<FString, UIWTDesignMerge::FExported>
UIWTDesignMerge::ExportedWidgets(const TSharedPtr<FJsonObject> &InExportSpec)
{
  TMap<FString, FExported> Result;
  const FTree Tree = FlattenSpec(InExportSpec);
  for (const TPair<FString, FFlat> &Pair : Tree.Widgets)
  {
    FExported Exported;
    if (Pair.Value.Props.IsValid())
    {
      Exported.Props =
          UIWTWidgetSpec::NormalizeValue(MakeShared<FJsonValueObject>(Pair.Value.Props))->AsObject();
    }
    if (Pair.Value.Slot.IsValid())
    {
      Exported.Slot =
          UIWTWidgetSpec::NormalizeValue(MakeShared<FJsonValueObject>(Pair.Value.Slot))->AsObject();
    }
    Result.Add(Pair.Key, MoveTemp(Exported));
  }
  return Result;
}

TMap<FString, UIWTDesignMerge::FExported>
UIWTDesignMerge::NextExported(const TMap<FString, FExported> &InAfterApply,
                              const TMap<FString, FBaseWidget> &InBase,
                              const TMap<FString, FKeptValues> &InKept)
{
  // InTarget's InKey as InFrom has it (or not at all).
  auto CarryOver = [](TSharedPtr<FJsonObject> &InOutTarget, const TSharedPtr<FJsonObject> &InFrom,
                      const FString &InKey)
  {
    const TSharedPtr<FJsonValue> Value = Field(InFrom, InKey);
    if (!InOutTarget.IsValid())
    {
      InOutTarget = MakeShared<FJsonObject>();
    }
    if (Value.IsValid())
    {
      InOutTarget->SetField(InKey, Value);
    }
    else
    {
      InOutTarget->RemoveField(InKey);
    }
  };

  TMap<FString, FExported> Result;
  for (const TPair<FString, FExported> &Pair : InAfterApply)
  {
    FExported Next;
    Next.Props = Pair.Value.Props.IsValid() ? TSharedPtr<FJsonObject>(Copy(Pair.Value.Props)) : nullptr;
    Next.Slot = Pair.Value.Slot.IsValid() ? TSharedPtr<FJsonObject>(Copy(Pair.Value.Slot)) : nullptr;
    const FKeptValues *Kept = InKept.Find(Pair.Key);
    const FBaseWidget *Old = InBase.Find(Pair.Key);
    if (Kept && Old && Old->Exported.IsSet())
    {
      const FExported &Before = *Old->Exported;
      if (Kept->bWhole)
      {
        Next.Props = Before.Props;
      }
      else
      {
        for (const FString &Key : Kept->Props)
        {
          CarryOver(Next.Props, Before.Props, Key);
        }
      }
      if (Kept->bWhole || Kept->bSlotWhole)
      {
        Next.Slot = Before.Slot;
      }
      else
      {
        for (const FString &Key : Kept->Slot)
        {
          CarryOver(Next.Slot, Before.Slot, Key);
        }
      }
    }
    Result.Add(Pair.Key, MoveTemp(Next));
  }
  return Result;
}
