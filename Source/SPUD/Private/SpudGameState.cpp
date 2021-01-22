#include "SpudGameState.h"

#include "EngineUtils.h"
#include "ISpudObject.h"
#include "SpudPropertyUtil.h"
#include "Engine/LevelStreaming.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY(LogSpudGameState)

//PRAGMA_DISABLE_OPTIMIZATION

USpudGameState::USpudGameState()
{
}

void USpudGameState::ResetState()
{
	SaveData.Reset();
}


void USpudGameState::UpdateFromWorld(UWorld* World)
{
	UpdateFromWorldImpl(World, false);
}

void USpudGameState::UpdateFromLevel(UWorld* World, const FString& LevelName)
{
	UpdateFromWorldImpl(World, true, LevelName);
}

void USpudGameState::UpdateFromWorldImpl(UWorld* World, bool bSingleLevel, const FString& OnlyLevel)
{
	SaveData.GlobalData.CurrentLevel = World->GetFName().ToString();

	// Persistent level AND streaming levels that are loaded show up in World->GetLevels
	for (auto && Level : World->GetLevels())
	{
		if (!bSingleLevel || GetLevelName(Level) == OnlyLevel)
		{
			UpdateFromLevel(Level);
		}
	}
}


void USpudGameState::UpdateFromLevel(ULevel* Level)
{
	const FString LevelName = GetLevelName(Level);
	auto LevelData = GetLevelData(LevelName, true);

	// Clear any existing data for levels being updated from
	// Which is either the specific level, or all loaded levels
	if (LevelData)
		LevelData->PreUpdateFromWorld();

	for (auto Actor : Level->Actors)
	{
		if (SpudPropertyUtil::IsPersistentObject(Actor))
		{
			UpdateFromActor(Actor, LevelData);
		}					
	}
}

USpudGameState::UpdateFromPropertyVisitor::UpdateFromPropertyVisitor(
	FSpudClassDef& InClassDef, TArray<uint32>& InPropertyOffsets,
	FSpudClassMetadata& InMeta, FMemoryWriter& InOut):
	ClassDef(InClassDef),
	PropertyOffsets(InPropertyOffsets),
	Meta(InMeta),
	Out(InOut)
{
}

bool USpudGameState::UpdateFromPropertyVisitor::VisitProperty(UObject* RootObject, FProperty* Property,
                                                                    uint32 CurrentPrefixID, void* ContainerPtr,
                                                                    int Depth)
{
	SpudPropertyUtil::UpdateFromProperty(RootObject, Property, CurrentPrefixID, ContainerPtr, Depth, ClassDef, PropertyOffsets, Meta, Out);
	return true;
}

void USpudGameState::UpdateFromPropertyVisitor::UnsupportedProperty(UObject* RootObject,
    FProperty* Property, uint32 CurrentPrefixID, int Depth)
{
	UE_LOG(LogSpudGameState, Error, TEXT("Property %s/%s is marked for save but is an unsupported type, ignoring. E.g. Arrays of custom structs are not supported."),
        *RootObject->GetName(), *Property->GetName());
	
}

uint32 USpudGameState::UpdateFromPropertyVisitor::GetNestedPrefix(
	FStructProperty* SProp, uint32 CurrentPrefixID)
{
	// When updating we generate new prefix IDs as needed
	return SpudPropertyUtil::FindOrAddNestedPrefixID(CurrentPrefixID, SProp, Meta);
}

void USpudGameState::WriteCoreActorData(AActor* Actor, FArchive& Out) const
{
	// Save core information which isn't in properties
	// We write this as packed data

	// Version: this needs to be incremented if any changes
	constexpr uint16 CoreDataVersion = 1;

	// Current Format:
	// - Version (uint16)
	// - Hidden (bool)
	// - Transform (FTransform)
	// - Velocity (FVector)
	// - AngularVelocity (FVector)

	// We could omit some of this data for non-movables but it's simpler to include for all

	SpudPropertyUtil::WriteRaw(CoreDataVersion, Out);

	SpudPropertyUtil::WriteRaw(Actor->IsHidden(), Out);
	SpudPropertyUtil::WriteRaw(Actor->GetTransform(), Out);
	
	FVector Velocity = FVector::ZeroVector;
	FVector AngularVelocity = FVector::ZeroVector;

	const auto RootComp = Actor->GetRootComponent();
	if (RootComp && RootComp->Mobility == EComponentMobility::Movable &&
		RootComp->IsSimulatingPhysics())
	{
		if (const auto &PrimComp = Cast<UPrimitiveComponent>(RootComp))
		{
			Velocity = Actor->GetVelocity();
			AngularVelocity = PrimComp->GetPhysicsAngularVelocityInDegrees();
		}
	}
	SpudPropertyUtil::WriteRaw(Velocity, Out);
	SpudPropertyUtil::WriteRaw(AngularVelocity, Out);
	
}

FString USpudGameState::GetLevelName(const ULevel* Level)
{
	// FName isn't good enough, it's "PersistentLevel" rather than the actual map name
	// Using the Outer to get the package name does it, same as for any other object
	return GetLevelNameForObject(Level);
}
FString USpudGameState::GetLevelNameForObject(const UObject* Obj)
{
	// Detect what level an object originated from
	// GetLevel()->GetName / GetFName() returns "PersistentLevel" all the time
	// GetLevel()->GetPathName returns e.g. /Game/Maps/[UEDPIE_0_]TestAdventureMap.TestAdventureMap:PersistentLevel
	// Outer is "PersistentLevel"
	// Outermost is "/Game/Maps/[UEDPIE_0_]TestAdventureStream0" so that's what we want
	const auto OuterMost = Obj->GetOutermost();
	if (OuterMost)
	{
		FString LevelName;
		OuterMost->GetName().Split("/", nullptr, &LevelName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		// Strip off PIE prefix, "UEDPIE_N_" where N is a number
		if (LevelName.StartsWith("UEDPIE_"))
			LevelName = LevelName.Right(LevelName.Len() - 9);
		return LevelName;
	}
	else
	{
		return FString();
	}	
}

FSpudLevelData* USpudGameState::GetLevelData(const FString& LevelName, bool AutoCreate)
{
	auto Ret = SaveData.LevelDataMap.Contents.Find(LevelName);
	if (!Ret && AutoCreate)
	{
		Ret = &SaveData.LevelDataMap.Contents.Add(LevelName);
		Ret->Name = LevelName;
	}
	
	return Ret;
}

FSpudNamedObjectData* USpudGameState::GetLevelActorData(const AActor* Actor, FSpudLevelData* LevelData, bool AutoCreate)
{
	// FNames are constant within a level
	const auto Name = SpudPropertyUtil::GetLevelActorName(Actor);
	FSpudNamedObjectData* Ret = LevelData->LevelActors.Contents.Find(Name);

	if (!Ret && AutoCreate)
	{
		Ret = &LevelData->LevelActors.Contents.Add(Name);
		Ret->Name = Name;
	}
	
	return Ret;
}

FString USpudGameState::GetClassName(const UObject* Obj)
{
	// Full class name allows for re-spawning
	// E.g. /Game/Blueprints/Class.Blah_C
	return Obj->GetClass()->GetPathName();
}

FSpudSpawnedActorData* USpudGameState::GetSpawnedActorData(AActor* Actor, FSpudLevelData* LevelData, bool AutoCreate)
{
	// For automatically spawned singleton objects such as GameModes, Pawns you should create a SpudGuid
	// property which you generate statically (not at construction), e.g. in the BP default value.
	// This way we can update its values and not have to re-spawn it.
	// Actually dynamically spawned items can be re-spawned if not there.
	
	// We need a GUID for runtime spawned actors
	FGuid Guid = SpudPropertyUtil::GetGuidProperty(Actor);
	bool GuidOk = Guid.IsValid();
	if (!GuidOk && AutoCreate)
	{
		// Create a new Guid to save data with
		// Provided there's a property to save it in
		Guid = FGuid::NewGuid();
		GuidOk = SpudPropertyUtil::SetGuidProperty(Actor, Guid);
	}
	
	if (!GuidOk)
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Ignoring runtime actor %s, missing or blank SpudGuid property"), *Actor->GetName())
		UE_LOG(LogSpudGameState, Error, TEXT("  Runtime spawned actors should have a SpudGuid property to identify them, initialised to valid unique value."))
		UE_LOG(LogSpudGameState, Error, TEXT("  NOTE: If this actor is part of a level and not runtime spawned, the cause of this false detection might be that you haven't SAVED the level before playing in the editor."))

		// TODO: if a class is a level object but happens to have a SpudGuid property anyway (maybe because sometimes runtime)
		// the lack of a level save making it look like a runtime object cannot be detected. Can we *maybe* call editor code somehow
		// to determine this?
		return nullptr;			
	}
	
	const auto GuidStr = Guid.ToString(SPUDDATA_GUID_KEY_FORMAT);
	FSpudSpawnedActorData* Ret = LevelData->SpawnedActors.Contents.Find(GuidStr);
	if (!Ret && AutoCreate)
	{
		Ret = &LevelData->SpawnedActors.Contents.Emplace(GuidStr);
		Ret->Guid = Guid;
		const FString ClassName = GetClassName(Actor); 
		Ret->ClassID = LevelData->Metadata.FindOrAddClassIDFromName(ClassName);
	}
	
	return Ret;
}

void USpudGameState::UpdateFromActor(AActor* Obj)
{
	if (Obj->HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject|RF_BeginDestroyed))
		return;

	const FString LevelName = GetLevelNameForObject(Obj);

	FSpudLevelData* LevelData = GetLevelData(LevelName, true);
	UpdateFromActor(Obj, LevelData);
		
}

void USpudGameState::UpdateLevelActorDestroyed(AActor* Actor)
{
	const FString LevelName = GetLevelNameForObject(Actor);

	FSpudLevelData* LevelData = GetLevelData(LevelName, true);
	UpdateLevelActorDestroyed(Actor, LevelData);
}

FSpudNamedObjectData* USpudGameState::GetGlobalObjectData(const UObject* Obj, bool AutoCreate)
{
	// Get the identifier; prefer GUID if present, if not just use name
	const FString ID = SpudPropertyUtil::GetGlobalObjectID(Obj);

	return GetGlobalObjectData(ID, AutoCreate);
}

FSpudNamedObjectData* USpudGameState::GetGlobalObjectData(const FString& ID, bool AutoCreate)
{
	FSpudNamedObjectData* Ret = SaveData.GlobalData.Objects.Contents.Find(ID);
	if (!Ret && AutoCreate)
	{
		Ret = &SaveData.GlobalData.Objects.Contents.Add(ID);
		Ret->Name = ID;
	}

	return Ret;
}


void USpudGameState::UpdateFromGlobalObject(UObject* Obj)
{
	UpdateFromGlobalObject(Obj, GetGlobalObjectData(Obj, true));
}

void USpudGameState::UpdateFromGlobalObject(UObject* Obj, const FString& ID)
{
	UpdateFromGlobalObject(Obj, GetGlobalObjectData(ID, true));
}

void USpudGameState::UpdateFromGlobalObject(UObject* Obj, FSpudNamedObjectData* Data)
{
	
	if (Data)
	{
		FSpudClassMetadata& Meta = SaveData.GlobalData.Metadata;
		const FString& ClassName = GetClassName(Obj);
		auto& ClassDef = Meta.FindOrAddClassDef(ClassName);
		auto& PropOffsets = Data->Properties.PropertyOffsets;
		
		auto& PropData = Data->Properties.Data;
		bool bIsCallback = Obj->GetClass()->ImplementsInterface(USpudObjectCallback::StaticClass());

		UE_LOG(LogSpudGameState, Verbose, TEXT("* Global object: %s"), *Obj->GetName());

		if (bIsCallback)
			ISpudObjectCallback::Execute_SpudPreSaveState(Obj, this);

		PropData.Empty();
		FMemoryWriter PropertyWriter(PropData);

		// visit all properties and write out
		UpdateFromPropertyVisitor Visitor(ClassDef, PropOffsets, Meta, PropertyWriter);
		SpudPropertyUtil::VisitPersistentProperties(Obj, Visitor);
		
		if (bIsCallback)
		{
			Data->CustomData.Data.Empty();
			FMemoryWriter CustomDataWriter(Data->CustomData.Data);
			auto CustomDataStruct = NewObject<USpudGameStateCustomData>();
			CustomDataStruct->Init(&CustomDataWriter);
			ISpudObjectCallback::Execute_SpudFinaliseSaveState(Obj, this, CustomDataStruct);
			
			ISpudObjectCallback::Execute_SpudPostSaveState(Obj, this);
		}
		
	}

	
	
}

void USpudGameState::RestoreLevel(UWorld* World, const FString& LevelName)
{
	RestoreLoadedWorld(World, true, LevelName);
}

void USpudGameState::RestoreLevel(ULevel* Level)
{
	if (!IsValid(Level))
		return;
	
	FString LevelName = GetLevelName(Level);
	FSpudLevelData* LevelData = GetLevelData(LevelName, false);

	if (!LevelData)
	{
		UE_LOG(LogSpudGameState, Warning, TEXT("Unable to restore level %s because data is missing"), *LevelName);
		return;
	}

	TMap<FGuid, UObject*> RuntimeObjectsByGuid;
	// Respawn dynamic actors first; they need to exist in order for cross-references in level actors to work
	for (auto&& SpawnedActor : LevelData->SpawnedActors.Contents)
	{
		auto Actor = RespawnActor(SpawnedActor.Value, LevelData->Metadata, Level);
		if (Actor)
			RuntimeObjectsByGuid.Add(SpawnedActor.Value.Guid, Actor);
		// Spawned actors will have been added to Level->Actors, their state will be restored there
	}
	// Restore existing actor state
	for (auto Actor : Level->Actors)
	{
		if (SpudPropertyUtil::IsPersistentObject(Actor))
		{
			RestoreActor(Actor, LevelData, &RuntimeObjectsByGuid);
			auto Guid = SpudPropertyUtil::GetGuidProperty(Actor);
			if (Guid.IsValid())
			{
				RuntimeObjectsByGuid.Add(Guid, Actor);
			}
		}
	}
	// Destroy actors in level but missing from save state
	for (auto&& DestroyedActor : LevelData->DestroyedActors.Values)
	{
		DestroyActor(DestroyedActor, Level);			
	}

}

void USpudGameState::RestoreActor(AActor* Actor)
{
	if (Actor->HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject|RF_BeginDestroyed))
		return;

	const FString LevelName = GetLevelNameForObject(Actor);

	FSpudLevelData* LevelData = GetLevelData(LevelName, false);
	if (!LevelData)
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Unable to restore Actor %s, missing level data"), *Actor->GetName());
		return;
	}

	RestoreActor(Actor, LevelData, nullptr);
}


AActor* USpudGameState::RespawnActor(const FSpudSpawnedActorData& SpawnedActor,
                                           const FSpudClassMetadata& Meta,
                                           ULevel* Level)
{
	const FString ClassName = Meta.GetClassNameFromID(SpawnedActor.ClassID);
	const FSoftClassPath CP(ClassName);
	const auto Class = CP.TryLoadClass<AActor>();

	if (!Class)
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Cannot respawn instance of %s, class not found"), *ClassName);
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.OverrideLevel = Level;
	// Important to spawn using level's world, our GetWorld may not be valid it turns out
	auto World = Level->GetWorld();
	AActor* Actor = World->SpawnActor<AActor>(Class, Params);
	if (Actor)
	{
		if (!SpudPropertyUtil::SetGuidProperty(Actor, SpawnedActor.Guid))
		{
			UE_LOG(LogSpudGameState, Error, TEXT("Re-spawned a runtime actor of class %s but it is missing a SpudGuid property!"), *ClassName);
		}		
	}
	return Actor;
}

void USpudGameState::DestroyActor(const FSpudDestroyedLevelActor& DestroyedActor, ULevel* Level)
{
	// We only ever have to destroy level actors, not runtime objects (those are just missing on restore)
	auto Obj = StaticFindObject(AActor::StaticClass(), Level, *DestroyedActor.Name);
	if (auto Actor = Cast<AActor>(Obj))
	{
		Level->GetWorld()->DestroyActor(Actor);
	}
}

bool USpudGameState::ShouldRespawnRuntimeActor(const AActor* Actor) const
{
	ESpudRespawnMode RespawnMode = ESpudRespawnMode::Default;
	// I know this cast style only supports C++ not Blueprints, but this method can only be defined in C++ anyway
	if (auto IObj = Cast<ISpudObject>(Actor))
	{
		RespawnMode = IObj->GetSpudRespawnMode();
	}

	switch (RespawnMode)
	{
	default:
	case ESpudRespawnMode::Default:
		// Default behaviour is to respawn everything except pawns, characters, game modes, game states
		// Those we assume are created by other init processes
		return !Actor->IsA(AGameModeBase::StaticClass()) &&
            !Actor->IsA(AGameStateBase::StaticClass()) &&
            !Actor->IsA(APawn::StaticClass()) &&
            !Actor->IsA(ACharacter::StaticClass());
	case ESpudRespawnMode::AlwaysRespawn:
		return true;
	case ESpudRespawnMode::NeverRespawn:
		return false;
	}
}


bool USpudGameState::ShouldActorBeRespawnedOnRestore(AActor* Actor) const
{
	return SpudPropertyUtil::IsRuntimeActor(Actor) &&
		ShouldRespawnRuntimeActor(Actor);
}


void USpudGameState::RestoreActor(AActor* Actor, FSpudLevelData* LevelData, const TMap<FGuid, UObject*>* RuntimeObjects)
{
	if (Actor->HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject|RF_BeginDestroyed))
		return;

	const bool bRespawned = ShouldActorBeRespawnedOnRestore(Actor);
	const FSpudObjectData* ActorData;

	if (bRespawned)
		ActorData = GetSpawnedActorData(Actor, LevelData, false);		
	else
		ActorData = GetLevelActorData(Actor, LevelData, false);

	if (ActorData)
	{
		UE_LOG(LogSpudGameState, Verbose, TEXT("Restoring Actor %s"), *Actor->GetName())
		PreRestoreObject(Actor);
		
		RestoreCoreActorData(Actor, ActorData->CoreData);
		RestoreObjectProperties(Actor, ActorData->Properties, LevelData->Metadata, RuntimeObjects);

		PostRestoreObject(Actor, ActorData->CustomData);		
	}
}


void USpudGameState::PreRestoreObject(UObject* Obj)
{
	if(Obj->GetClass()->ImplementsInterface(USpudObjectCallback::StaticClass()))
	{
		ISpudObjectCallback::Execute_SpudPreLoadState(Obj, this);
		
	}
}

void USpudGameState::PostRestoreObject(UObject* Obj, const FSpudCustomData& FromCustomData)
{
	if (Obj->GetClass()->ImplementsInterface(USpudObjectCallback::StaticClass()))
	{
		FMemoryReader Reader(FromCustomData.Data);
		auto CustomData = NewObject<USpudGameStateCustomData>();
		CustomData->Init(&Reader);
		ISpudObjectCallback::Execute_SpudFinaliseLoadState(Obj, this, CustomData);
		ISpudObjectCallback::Execute_SpudPostLoadState(Obj, this);
	}
}

void USpudGameState::RestoreCoreActorData(AActor* Actor, const FSpudCoreActorData& FromData)
{
	// Restore core data based on version
	// Unlike properties this is packed data, versioned

	FMemoryReader In(FromData.Data);
	
	// All formats have version number first (this is separate from the file version)
	uint16 InVersion = 0;
	SpudPropertyUtil::ReadRaw(InVersion, In);

	if (InVersion == 1)
	{
		// First, and only version right now
		// V1 Format:
		// - Version (uint16)
		// - Hidden (bool)
		// - Transform (FTransform)
		// - Velocity (FVector)
		// - AngularVelocity (FVector)

		bool Hidden;
		SpudPropertyUtil::ReadRaw(Hidden, In);
		Actor->SetActorHiddenInGame(Hidden);

		FTransform XForm;
		SpudPropertyUtil::ReadRaw(XForm, In);
		Actor->SetActorTransform(XForm);

		FVector Velocity, AngularVelocity;
		SpudPropertyUtil::ReadRaw(Velocity, In);
		SpudPropertyUtil::ReadRaw(AngularVelocity, In);
		const auto RootComp = Actor->GetRootComponent();
		if (RootComp && RootComp->Mobility == EComponentMobility::Movable &&
            RootComp->IsSimulatingPhysics())
		{
			if (const auto &PrimComp = Cast<UPrimitiveComponent>(RootComp))
			{
				PrimComp->SetPhysicsLinearVelocity(Velocity);
				PrimComp->SetPhysicsAngularVelocityInDegrees(AngularVelocity);
			}
		}
	}
	else
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Core Actor Data for %s is corrupt, not restoring"), *Actor->GetName())
		return;
	}
}

void USpudGameState::RestoreObjectProperties(UObject* Obj, const FSpudPropertyData& FromData, const FSpudClassMetadata& Meta, const TMap<FGuid, UObject*>* RuntimeObjects)
{
	const auto ClassName = GetClassName(Obj);
	const auto ClassDef = Meta.GetClassDef(ClassName);
	if (!ClassDef)
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Unable to find ClassDef for: %s %s"), *GetClassName(Obj));
		return;
	}

	// We can use the "fast" path if the stored definition of the class properties exactly matches the runtime order
	// ClassDef caches the result of this across the context of one loaded file
	const bool bUseFastPath = ClassDef->MatchesRuntimeClass(Meta);	

	if (bUseFastPath)
		RestoreObjectPropertiesFast(Obj, FromData, Meta, ClassDef, RuntimeObjects);
	else
		RestoreObjectPropertiesSlow(Obj, FromData, Meta, ClassDef, RuntimeObjects);
}


void USpudGameState::RestoreObjectPropertiesFast(UObject* Obj, const FSpudPropertyData& FromData,
                                                       const FSpudClassMetadata& Meta,
                                                       const FSpudClassDef* ClassDef,
                                                       const TMap<FGuid, UObject*>* RuntimeObjects)
{
	UE_LOG(LogSpudGameState, Verbose, TEXT("Restoring %s properties via FAST path, %d properties"), *ClassDef->ClassName, ClassDef->Properties.Num());
	const auto StoredPropertyIterator = ClassDef->Properties.CreateConstIterator();

	FMemoryReader In(FromData.Data);
	RestoreFastPropertyVisitor Visitor(StoredPropertyIterator, In, *ClassDef, Meta, RuntimeObjects);
	SpudPropertyUtil::VisitPersistentProperties(Obj, Visitor);
	
}

void USpudGameState::RestoreObjectPropertiesSlow(UObject* Obj, const FSpudPropertyData& FromData,
                                                       const FSpudClassMetadata& Meta,
                                                       const FSpudClassDef* ClassDef,
                                                       const TMap<FGuid, UObject*>* RuntimeObjects)
{
	UE_LOG(LogSpudGameState, Verbose, TEXT("Restoring %s properties via SLOW path, %d properties"), *ClassDef->ClassName, ClassDef->Properties.Num());

	FMemoryReader In(FromData.Data);
	RestoreSlowPropertyVisitor Visitor(In, *ClassDef, Meta, RuntimeObjects);
	SpudPropertyUtil::VisitPersistentProperties(Obj, Visitor);
}


uint32 USpudGameState::RestorePropertyVisitor::GetNestedPrefix(FStructProperty* SProp, uint32 CurrentPrefixID)
{
	// This doesn't create a new ID, expects it to be there already (should be since restoring)
	return SpudPropertyUtil::GetNestedPrefixID(CurrentPrefixID, SProp, Meta);
}


bool USpudGameState::RestoreFastPropertyVisitor::VisitProperty(UObject* RootObject, FProperty* Property,
	uint32 CurrentPrefixID, void* ContainerPtr, int Depth)
{
	// Fast path can just iterate both sides of properties because stored properties are in the same order
	auto& StoredProperty = *StoredPropertyIterator;
	SpudPropertyUtil::RestoreProperty(RootObject, Property, ContainerPtr, StoredProperty, RuntimeObjects, DataIn);

	// We DON'T increment the property iterator for custom structs, since they don't have any values of their own
	// It's their nested properties that have the values, they're only context
	if (!SpudPropertyUtil::IsCustomStructProperty(Property))
		++StoredPropertyIterator;

	return true;
}

bool USpudGameState::RestoreSlowPropertyVisitor::VisitProperty(UObject* RootObject, FProperty* Property,
                                                                     uint32 CurrentPrefixID, void* ContainerPtr, int Depth)
{
	// This is the slow alternate property restoration path
	// Used when the runtime class definition no longer matches the stored class definition
	// This should go away as soon as the data is re-saved and go back to the fast path


	// Custom structs don't need to do anything at the root, visitor calls will cascade inside for each property inside the struct
	// Builtin structs continue though since those are restored with custom, more efficient member population
	if (SpudPropertyUtil::IsCustomStructProperty(Property))
		return true;
	
	// PropertyLookup is PrefixID -> Map of PropertyNameID to PropertyIndex
	auto InnerMapPtr = ClassDef.PropertyLookup.Find(CurrentPrefixID);
	if (!InnerMapPtr)
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Error in RestoreSlowPropertyVisitor, PrefixID invalid for %, class %s"), *Property->GetName(), *ClassDef.ClassName);
		return true;
	}
	
	uint32 PropID = Meta.GetPropertyIDFromName(Property->GetName());
	if (PropID == SPUDDATA_INDEX_NONE)
	{
		UE_LOG(LogSpudGameState, Warning, TEXT("Skipping property %s on class %s, not found in class definition"), *Property->GetName(), *ClassDef.ClassName);
		return true;
	}
	const int* PropertyIndexPtr = InnerMapPtr->Find(PropID);
	if (!PropertyIndexPtr)
	{
		UE_LOG(LogSpudGameState, Warning, TEXT("Skipping property %s on class %s, data not found"), *Property->GetName(), *ClassDef.ClassName);
		return true;		
	}
	if (*PropertyIndexPtr < 0 || *PropertyIndexPtr >= ClassDef.Properties.Num())
	{
		UE_LOG(LogSpudGameState, Error, TEXT("Error in RestoreSlowPropertyVisitor, invalid property index for %s on class %s"), *Property->GetName(), *ClassDef.ClassName);
		return true;		
	}
	auto& StoredProperty = ClassDef.Properties[*PropertyIndexPtr];
	
	SpudPropertyUtil::RestoreProperty(RootObject, Property, ContainerPtr, StoredProperty, RuntimeObjects, DataIn);
	return true;
}

void USpudGameState::RestoreLoadedWorld(UWorld* World)
{
	RestoreLoadedWorld(World, false);
}

void USpudGameState::RestoreLoadedWorld(UWorld* World, bool bSingleLevel, const FString& OnlyLevel)
{
	// So that we don't need to check every instance of a class for matching stored / runtime class properties
	// we will keep a cache of whether to use the fast or slow path. It's only valid for this specific load
	// because we may load level data or different ages
	for (auto& Level : World->GetLevels())
	{
		// Null levels possible
		if (!IsValid(Level))
			continue;

		if (bSingleLevel && GetLevelName(Level) != OnlyLevel)
			continue;

		RestoreLevel(Level);
		
	}

}

void USpudGameState::RestoreGlobalObject(UObject* Obj)
{
	RestoreGlobalObject(Obj, GetGlobalObjectData(Obj, false));
}

void USpudGameState::RestoreGlobalObject(UObject* Obj, const FString& ID)
{
	RestoreGlobalObject(Obj, GetGlobalObjectData(ID, false));
}

void USpudGameState::RestoreGlobalObject(UObject* Obj, const FSpudNamedObjectData* Data)
{
	if (Data)
	{
		UE_LOG(LogSpudGameState, Verbose, TEXT("Restoring Global Object %s"), *Data->Name)
		PreRestoreObject(Obj);
		
		RestoreObjectProperties(Obj, Data->Properties, SaveData.GlobalData.Metadata, nullptr);

		PostRestoreObject(Obj, Data->CustomData);
	}
	
}
void USpudGameState::UpdateFromActor(AActor* Actor, FSpudLevelData* LevelData)
{
	if (Actor->HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject|RF_BeginDestroyed))
		return;

	// GetUniqueID() is unique in the current play session but not across games
	// GetFName() is unique within a level, and stable for objects loaded from a level
	// For runtime created objects we need another stable GUID
	// For that we'll rely on a SpudGuid property
	// For convenience you can use one of the persistent base classes to get that, otherwise you need
	// to add a SpudGuid propery
	
	// This is how we identify run-time created objects
	bool bRespawn = ShouldActorBeRespawnedOnRestore(Actor);
	FString Name;
	FGuid Guid;

	TArray<uint8>* pDestCoreData = nullptr;
	TArray<uint8>* pDestPropertyData = nullptr;
	TArray<uint8>* pDestCustomData = nullptr;
	FSpudClassMetadata& Meta = LevelData->Metadata;
	FSpudClassDef& ClassDef = Meta.FindOrAddClassDef(GetClassName(Actor));
	TArray<uint32>* pOffsets = nullptr;
	if (bRespawn)
	{
		auto ActorData = GetSpawnedActorData(Actor, LevelData, true);
		if (ActorData)
		{
			pDestCoreData = &ActorData->CoreData.Data;
			pDestPropertyData = &ActorData->Properties.Data;
			pDestCustomData = &ActorData->CustomData.Data;
			pOffsets = &ActorData->Properties.PropertyOffsets;
			Guid = ActorData->Guid;
			Name = SpudPropertyUtil::GetLevelActorName(Actor);
		}
	}
	else
	{
		auto ActorData = GetLevelActorData(Actor, LevelData, true);
		if (ActorData)
		{
			pDestCoreData = &ActorData->CoreData.Data;
			pDestPropertyData = &ActorData->Properties.Data;
			pDestCustomData = &ActorData->CustomData.Data;
			pOffsets = &ActorData->Properties.PropertyOffsets;
			Name = ActorData->Name;
		}
	}

	if (!pDestPropertyData || !pOffsets)
	{
		// Something went wrong, we'll assume the detail has been logged elsewhere
		return;	
	}
	

	if (bRespawn)
		UE_LOG(LogSpudGameState, Verbose, TEXT("* Runtime object: %s (%s)"), *Guid.ToString(EGuidFormats::DigitsWithHyphens), *Name)
	else
		UE_LOG(LogSpudGameState, Verbose, TEXT("* Level object: %s/%s"), *LevelData->Name, *Name);

	pDestPropertyData->Empty();
	FMemoryWriter PropertyWriter(*pDestPropertyData);

	bool bIsCallback = Actor->GetClass()->ImplementsInterface(USpudObjectCallback::StaticClass());

	if (bIsCallback)
		ISpudObjectCallback::Execute_SpudPreSaveState(Actor, this);

	// Core data first
	pDestCoreData->Empty();
	FMemoryWriter CoreDataWriter(*pDestCoreData);
	WriteCoreActorData(Actor, CoreDataWriter);

	// Now properties, visit all and write out
	UpdateFromPropertyVisitor Visitor(ClassDef, *pOffsets, Meta, PropertyWriter);
	SpudPropertyUtil::VisitPersistentProperties(Actor, Visitor);

	if (bIsCallback)
	{
		if (pDestCustomData)
		{
			pDestCustomData->Empty();
			FMemoryWriter CustomDataWriter(*pDestCustomData);
			auto CustomDataStruct = NewObject<USpudGameStateCustomData>();
			CustomDataStruct->Init(&CustomDataWriter);
			ISpudObjectCallback::Execute_SpudFinaliseSaveState(Actor, this, CustomDataStruct);
		}			
	
		ISpudObjectCallback::Execute_SpudPostSaveState(Actor, this);
	}
}


void USpudGameState::UpdateLevelActorDestroyed(AActor* Actor, FSpudLevelData* LevelData)
{
	// We don't check for duplicates, because it should only be possible to destroy a uniquely named level actor once
	LevelData->DestroyedActors.Add(SpudPropertyUtil::GetLevelActorName(Actor));
}

void USpudGameState::SaveToArchive(FArchive& Ar, const FText& Title)
{
	// We use separate read / write in order to more clearly support chunked file format
	// with the backwards compatibility that comes with 
	auto ChunkedAr = FSpudChunkedDataArchive(Ar);
	SaveData.PrepareForWrite(Title);
	SaveData.WriteToArchive(ChunkedAr);

}

void USpudGameState::LoadFromArchive(FArchive& Ar)
{
	auto ChunkedAr = FSpudChunkedDataArchive(Ar);
	SaveData.ReadFromArchive(ChunkedAr, 0);	
}

bool USpudGameState::LoadSaveInfoFromArchive(FArchive& Ar, USpudSaveGameInfo& OutInfo)
{
	auto ChunkedAr = FSpudChunkedDataArchive(Ar);
	FSpudSaveInfo StorageInfo;
	const bool Ok = FSpudSaveData::ReadSaveInfoFromArchive(ChunkedAr, StorageInfo);
	if (Ok)
	{
		OutInfo.Title = StorageInfo.Title;
		OutInfo.Timestamp = StorageInfo.Timestamp;
	}
	return Ok;
	
}


//PRAGMA_ENABLE_OPTIMIZATION
