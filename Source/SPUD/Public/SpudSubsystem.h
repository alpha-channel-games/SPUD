#pragma once

#include "CoreMinimal.h"
#include "SpudGameState.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "SpudSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpudSubsystem, Verbose, Verbose);

UENUM(BlueprintType)
enum class ESpudSystemState : uint8
{
	/// Game is not currently running, no persistence
	Disabled,
	/// Game is running, persistence is enabled
	RunningIdle,
	/// Currently loading a save game, cannot be interrupted
    LoadingGame,
	/// Currently saving a game, cannot be interrupted
    SavingGame
};

/// Subsystem which controls our save games, and also the active game's persistent state (for streaming levels)
UCLASS()
class SPUD_API USpudSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
protected:
	FDelegateHandle OnPreLoadMapHandle;
	FDelegateHandle OnPostLoadMapHandle;
	int32 LoadUnloadRequests = 0;
	bool FirstStreamRequestSinceMapLoad = true;
	TMap<int32, FName> LevelsPendingLoad;

	UPROPERTY()
	TArray<TWeakObjectPtr<UObject>> GlobalObjects;
	UPROPERTY()
	TMap<FString, TWeakObjectPtr<UObject>> NamedGlobalObjects;
	
	UPROPERTY()
	TArray<USpudSaveGameInfo*> SaveGameList;
	
	UPROPERTY(BlueprintReadOnly)
	ESpudSystemState CurrentState = ESpudSystemState::RunningIdle;

	// The currently active game state
	UPROPERTY()
	USpudGameState* ActiveState;

	USpudGameState* GetActiveState()
	{
		if (!IsValid(ActiveState))
			ActiveState = NewObject<USpudGameState>();

		return ActiveState;
	}
	
	// Map of level names to the list of objects which requested them, making it easier to unload once all
	// requesters have withdrawn
	TMap<FName, TArray<TWeakObjectPtr<>>> LevelRequesters;


	UFUNCTION()
	void OnPreLoadMap(const FString& MapName);
	UFUNCTION()
	void OnPostLoadMap(UWorld* World);
	UFUNCTION()
	void OnActorDestroyed(AActor* Actor);
	void RefreshSaveGameList();
	void SubscribeAllLevelObjectEvents();
	void SubscribeLevelObjectEvents(ULevel* Level);
	void UnsubscribeLevelObjectEvents(ULevel* Level);
	void UnsubscribeAllLevelObjectEvents();
	
	// This is a latent callback and has to be BlueprintCallable
	UFUNCTION(BlueprintCallable)
	void PostLoadStreamLevel(int32 LinkID);
	UFUNCTION(BlueprintCallable)
    void PostUnloadStreamLevel(int32 LinkID);

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/// Start a new game with a blank persistent state
	UFUNCTION(BlueprintCallable)
    void NewGame();

	/// Terminate a running game. Does not save state. Call this when returning to a main menu for example.
	/// All map changes after this are ignored by the persistence system
	UFUNCTION(BlueprintCallable)
	void EndGame();

	/// Trigger an autosave of the game; this one will register as the latest save, but will NOT count as a Quick Save
	/// for Quick Load purposes
	UFUNCTION(BlueprintCallable)
    void AutoSaveGame();
	/// Perform a Quick Save of the game in a single re-used slot, in response to a player request
	UFUNCTION(BlueprintCallable)
    void QuickSaveGame();
	/// Quick load the game from the last player-requested Quick Save slot (NOT the last autosave or manual save)
	UFUNCTION(BlueprintCallable)
    void QuickLoadGame();
	/// Continue a game from the latest save of any kind - autosave, quick save, manual save. The same as calling LoadGame on the most recent. 
	UFUNCTION(BlueprintCallable)
    void LoadLatestSaveGame();


	/// Save the game in a given slot name, with an optional descriptive title
	UFUNCTION(BlueprintCallable)
    bool SaveGame(const FString& SlotName, const FText& Title = FText());
	/// Load the game in a given slot name
	UFUNCTION(BlueprintCallable)
    bool LoadGame(const FString& SlotName);

	/// Delete the save game in a given slot
	UFUNCTION(BlueprintCallable)
    bool DeleteSave(const FString& SlotName);

	/**
	* @brief Add a global object to the list of objects which will have their state saved / loaded
		Level actors which implement ISpudObject will automatically be saved/loaded but global objects like GameInstance
		will not be, by default. Call this to add additional objects to the save/load behaviour. 
	 * @param Obj The global object to track. This object will have its state saved/loaded.
	 * @note As a global object, Obj is expected to always exist and will not be re-created by this system. In addition,
	 * it must be uniquely identifiable, either by its FName, or by a SpudGuid property you add to it. If you cannot
	 * guarantee the FName is always the same (may not always be the case with auto-created objects like GameInstance)
	 * then either add a predefined SpudGuid (FGuid) property and always set it to a known value, or use the alternative
	 * AddPersistentGlobalObjectWithName method, which identifies this object explicitly.
	 */
	UFUNCTION(BlueprintCallable)
	void AddPersistentGlobalObject(UObject* Obj);
	
	/**
	* @brief Add a global object to the list of objects which will have their state saved / loaded
		Level actors which implement ISpudObject will automatically be saved/loaded but global objects like GameInstance
		will not be, by default. Call this to add additional objects to the save/load behaviour. 
	 * @param Obj The global object to track. This object will have its state saved/loaded.
	 * @param Name The name by which to identify this object in the save file. This is in case you can't guarantee the
	 * FName of the object is always the same (e.g. GameInstance_0, GameInstance_1 auto-generated names), so you always
	 * want this object to be identified by this name. You could also add a constant FGuid property called SpudGuid to
	 * identify the object, but this method means you don't have to do that.
	 */
	UFUNCTION(BlueprintCallable)
    void AddPersistentGlobalObjectWithName(UObject* Obj, const FString& Name);

	/**
	 * @brief Remove an object from tracking so it will no longer be saved or loaded.
	 * @param Obj The object to remove from tracking
	 */
	UFUNCTION(BlueprintCallable)
    void RemovePersistentGlobalObject(UObject* Obj);

	/// Make a request that a streaming level is loaded. Won't load if already loaded, but will
	/// record the request count so that unloading is done when all requests are withdrawn.
	UFUNCTION(BlueprintCallable)
	void AddRequestForStreamingLevel(UObject* Requester, FName LevelName, bool BlockingLoad);
	/// Withdraw a request for a streaming level. Once all requesters have rescinded their requests, the
	/// streaming level will be considered ready to be unloaded.
	UFUNCTION(BlueprintCallable)
	void WithdrawRequestForStreamingLevel(UObject* Requester, FName LevelName);

	/// Get the list of the save games with metadata
	UFUNCTION(BlueprintCallable)
	const TArray<USpudSaveGameInfo*>& GetSaveGameList();

	/// Get info about the latest save game
	UFUNCTION(BlueprintCallable)
	USpudSaveGameInfo* GetLatestSaveGame();

	/// By default you're not allowed to interrupt save / load operations and any requests received while another is
	/// believed to be ongoing is ignored. This method basically overrides this and tells the system to accept
	/// new requests regardless. This is a last resort that you should never need, but it's here as a safety valve in case of errors.
	UFUNCTION(BlueprintCallable)
    void ForceReset();

	/// Load a streaming level by name, correctly handling state restoration
	void LoadStreamLevel(FName LevelName, bool Blocking);
	/// Unload a streaming level by name, saving state beforehand
	void UnloadStreamLevel(FName LevelName);

	static FString GetSaveGameDirectory();
	static FString GetSaveGameFilePath(const FString& SlotName);
	static FString GetActiveGameFolder();
	static FString GetActiveGameFilePath(const FString& Name);
	
};

inline USpudSubsystem* GetSpudSubsystem(UWorld* WorldContext)
{
	if (IsValid(WorldContext) && WorldContext->IsGameWorld())
	{
		auto GI = WorldContext->GetGameInstance();
		if (IsValid(GI))
			return GI->GetSubsystem<USpudSubsystem>();		
	}
		
	return nullptr;
}