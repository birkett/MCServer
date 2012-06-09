
#include "Globals.h"  // NOTE: MSVC stupidness requires this to be the same across all modules

#include "BlockID.h"
#include "cWorld.h"
#include "cRedstone.h"
#include "ChunkDef.h"
#include "cClientHandle.h"
#include "cPickup.h"
#include "cBlockToPickup.h"
#include "cPlayer.h"
#include "cServer.h"
#include "cItem.h"
#include "cRoot.h"
#include "../iniFile/iniFile.h"
#include "cChunkMap.h"
#include "cSimulatorManager.h"
#include "cWaterSimulator.h"
#include "cLavaSimulator.h"
#include "cFireSimulator.h"
#include "cSandSimulator.h"
#include "cRedstoneSimulator.h"
#include "cChicken.h"
#include "cSpider.h"
#include "cCow.h" //cow
#include "cSquid.h" //Squid
#include "cWolf.h" //wolf
#include "cSlime.h" //slime
#include "cSkeleton.h" //Skeleton
#include "cSilverfish.h" //Silverfish
#include "cPig.h" //pig
#include "cSheep.h" //sheep
#include "cZombie.h" //zombie
#include "cEnderman.h" //enderman
#include "cCreeper.h" //creeper
#include "cCavespider.h" //cavespider
#include "cGhast.h" //Ghast
#include "cZombiepigman.h" //Zombiepigman
#include "cMakeDir.h"
#include "cChunkGenerator.h"
#include "MersenneTwister.h"
#include "cTracer.h"
#include "Trees.h"


#include "packets/cPacket_TimeUpdate.h"
#include "packets/cPacket_NewInvalidState.h"
#include "packets/cPacket_Thunderbolt.h"

#include "Vector3d.h"

#include <time.h>

#include "tolua++.h"

#ifndef _WIN32
	#include <stdlib.h>
#endif





/// Up to this many m_SpreadQueue elements are handled each world tick
const int MAX_LIGHTING_SPREAD_PER_TICK = 10;





float cWorld::m_Time = 0.f;





///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cWorldLoadProgress:

/// A simple thread that displays the progress of world loading / saving in cWorld::InitializeSpawn()
class cWorldLoadProgress :
	public cIsThread
{
public:
	cWorldLoadProgress(cWorld * a_World) :
		cIsThread("cWorldLoadProgress"),
		m_World(a_World)
	{
		Start();
	}
	
	void Stop(void)
	{
		m_ShouldTerminate = true;
		Wait();
	}
	
protected:

	cWorld * m_World;
	
	virtual void Execute(void) override
	{
		for (;;)
		{
			LOG("%d chunks to load, %d chunks to generate", 
				m_World->GetStorage().GetLoadQueueLength(),
				m_World->GetGenerator().GetQueueLength()
			);
			
			// Wait for 2 sec, but be "reasonably wakeable" when the thread is to finish
			for (int i = 0; i < 20; i++)
			{
				cSleep::MilliSleep(100);
				if (m_ShouldTerminate)
				{
					return;
				}
			}
		}  // for (-ever)
	}
	
} ;





/// A simple thread that displays the progress of world lighting in cWorld::InitializeSpawn()
class cWorldLightingProgress :
	public cIsThread
{
public:
	cWorldLightingProgress(cLightingThread * a_Lighting) :
		cIsThread("cWorldLightingProgress"),
		m_Lighting(a_Lighting)
	{
		Start();
	}
	
	void Stop(void)
	{
		m_ShouldTerminate = true;
		Wait();
	}
	
protected:

	cLightingThread * m_Lighting;
	
	virtual void Execute(void) override
	{
		for (;;)
		{
			LOG("%d chunks remaining to light", m_Lighting->GetQueueLength()
			);
			
			// Wait for 2 sec, but be "reasonably wakeable" when the thread is to finish
			for (int i = 0; i < 20; i++)
			{
				cSleep::MilliSleep(100);
				if (m_ShouldTerminate)
				{
					return;
				}
			}
		}  // for (-ever)
	}
	
} ;





///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cWorld:

cWorld* cWorld::GetWorld()
{
	LOGWARN("WARNING: Using deprecated function cWorld::GetWorld() use cRoot::Get()->GetDefaultWorld() instead!");
	return cRoot::Get()->GetDefaultWorld();
}





cWorld::~cWorld()
{
	{
		cCSLock Lock(m_CSEntities);
		while( m_AllEntities.begin() != m_AllEntities.end() )
		{
			cEntity* Entity = *m_AllEntities.begin();
			m_AllEntities.remove( Entity );
			if ( !Entity->IsDestroyed() )
			{
				Entity->Destroy();
			}
			delete Entity;
		}
	}

	delete m_SimulatorManager;
	delete m_SandSimulator;
	delete m_WaterSimulator;
	delete m_LavaSimulator;
	delete m_FireSimulator;
	delete m_RedstoneSimulator;

	m_Generator.Stop();
	m_ChunkSender.Stop();

	UnloadUnusedChunks();
	
	m_Storage.WaitForFinish();

	delete m_ChunkMap;
}





cWorld::cWorld( const AString & a_WorldName )
	: m_SpawnMonsterTime( 0.f )
	, m_RSList ( 0 )
	, m_Weather ( eWeather_Sunny )
{
	LOG("cWorld::cWorld(%s)", a_WorldName.c_str());
	m_WorldName = a_WorldName;
	m_IniFileName = m_WorldName + "/world.ini";

	cMakeDir::MakeDir(m_WorldName.c_str());

	MTRand r1;
	m_SpawnX = (double)((r1.randInt() % 1000) - 500);
	m_SpawnY = cChunkDef::Height;
	m_SpawnZ = (double)((r1.randInt() % 1000) - 500);
	m_GameMode = eGameMode_Creative;

	AString StorageSchema("Default");

	cIniFile IniFile(m_IniFileName);
	if (IniFile.ReadFile())
	{
		m_SpawnX = IniFile.GetValueF("SpawnPosition", "X", m_SpawnX);
		m_SpawnY = IniFile.GetValueF("SpawnPosition", "Y", m_SpawnY);
		m_SpawnZ = IniFile.GetValueF("SpawnPosition", "Z", m_SpawnZ);
		m_GameMode = (eGameMode)IniFile.GetValueI("GameMode", "GameMode", m_GameMode );
		StorageSchema = IniFile.GetValue("Storage", "Schema", StorageSchema);
		m_MaxCactusHeight           = IniFile.GetValueI("Plants", "MaxCactusHeight",           3);
		m_MaxSugarcaneHeight        = IniFile.GetValueI("Plants", "MaxSugarcaneHeight",        3);
		m_IsCropsBonemealable       = IniFile.GetValueB("Plants", "IsCropsBonemealable",       true);
		m_IsGrassBonemealable       = IniFile.GetValueB("Plants", "IsGrassBonemealable",       true);
		m_IsSaplingBonemealable     = IniFile.GetValueB("Plants", "IsSaplingBonemealable",     true);
		m_IsMelonStemBonemealable   = IniFile.GetValueB("Plants", "IsMelonStemBonemealable",   true);
		m_IsMelonBonemealable       = IniFile.GetValueB("Plants", "IsMelonBonemealable",       false);
		m_IsPumpkinStemBonemealable = IniFile.GetValueB("Plants", "IsPumpkinStemBonemealable", true);
		m_IsPumpkinBonemealable     = IniFile.GetValueB("Plants", "IsPumpkinBonemealable",     false);
		m_IsSugarcaneBonemealable   = IniFile.GetValueB("Plants", "IsSugarcaneBonemealable",   false);
		m_IsCactusBonemealable      = IniFile.GetValueB("Plants", "IsCactusBonemealable",      false);
	}
	else
	{
		IniFile.SetValueF("SpawnPosition", "X", m_SpawnX );
		IniFile.SetValueF("SpawnPosition", "Y", m_SpawnY );
		IniFile.SetValueF("SpawnPosition", "Z", m_SpawnZ );
		IniFile.SetValueI("GameMode", "GameMode", m_GameMode );
		IniFile.SetValue("Storage", "Schema", StorageSchema);
		if( !IniFile.WriteFile() )
		{
			LOG("WARNING: Could not write to %s", m_IniFileName.c_str());
		}
	}
	
	m_Lighting.Start(this);
	m_Storage.Start(this, StorageSchema);
	m_Generator.Start(this, IniFile);

	m_bAnimals = true;
	m_SpawnMonsterRate = 10;
	cIniFile IniFile2("settings.ini");
	if( IniFile2.ReadFile() )
	{
		m_bAnimals = IniFile2.GetValueB("Monsters", "AnimalsOn", true );
		m_SpawnMonsterRate = (float)IniFile2.GetValueF("Monsters", "AnimalSpawnInterval", 10);
		SetMaxPlayers(IniFile2.GetValueI("Server", "MaxPlayers", 9001));
		m_Description = IniFile2.GetValue("Server", "Description", "MCServer! - It's OVER 9000!").c_str();
	}

	m_ChunkMap = new cChunkMap(this );
	
	m_ChunkSender.Start(this);

	m_Time = 0;
	m_WorldTimeFraction = 0.f;
	m_WorldTime = 0;
	m_LastSave = 0;
	m_LastUnload = 0;

	//Simulators:
	m_WaterSimulator = new cWaterSimulator( this );
	m_LavaSimulator = new cLavaSimulator( this );
	m_SandSimulator = new cSandSimulator(this);
	m_FireSimulator = new cFireSimulator(this);
	m_RedstoneSimulator = new cRedstoneSimulator(this);

	m_SimulatorManager = new cSimulatorManager();
	m_SimulatorManager->RegisterSimulator(m_WaterSimulator, 6);
	m_SimulatorManager->RegisterSimulator(m_LavaSimulator, 12);
	m_SimulatorManager->RegisterSimulator(m_SandSimulator, 1);
	m_SimulatorManager->RegisterSimulator(m_FireSimulator, 10);
	m_SimulatorManager->RegisterSimulator(m_RedstoneSimulator, 1);
}





void cWorld::SetWeather( eWeather a_Weather )
{
	switch( a_Weather )
	{
	case eWeather_Sunny:
		{
			m_Weather = a_Weather;
			cPacket_NewInvalidState WeatherPacket;
			WeatherPacket.m_Reason = 2; //stop rain
			Broadcast ( WeatherPacket );
		}
		break;
	case eWeather_Rain:
		{
			m_Weather = a_Weather;
			cPacket_NewInvalidState WeatherPacket;
			WeatherPacket.m_Reason = 1; //begin rain
			Broadcast ( WeatherPacket );
		}
		break;
	case eWeather_ThunderStorm:
		{
			m_Weather = a_Weather;
			cPacket_NewInvalidState WeatherPacket;
			WeatherPacket.m_Reason = 1; //begin rain
			Broadcast ( WeatherPacket );
			CastThunderbolt ( 0, 0, 0 ); //start thunderstorm with a lightning strike at 0, 0, 0. >:D
		}
		break;
	default:
		LOGWARN("Trying to set unknown weather %d", a_Weather );
		break;
	}
}





void cWorld::CastThunderbolt ( int a_X, int a_Y, int a_Z )
{
	cPacket_Thunderbolt ThunderboltPacket;
	ThunderboltPacket.m_xLBPos = a_X;
	ThunderboltPacket.m_yLBPos = a_Y;
	ThunderboltPacket.m_zLBPos = a_Z;
	Broadcast( ThunderboltPacket ); // FIXME: Broadcast to chunk instead of entire world
}





bool cWorld::IsPlacingItemLegal(Int16 a_ItemType, int a_BlockX, int a_BlockY, int a_BlockZ)
{
	BLOCKTYPE SurfaceBlock = GetBlock(a_BlockX, a_BlockY - 1, a_BlockZ);
	switch (a_ItemType)
	{
		case E_BLOCK_YELLOW_FLOWER:	// Can ONLY be placed on dirt/grass
		case E_BLOCK_RED_ROSE:
		case E_BLOCK_SAPLING:
		{
			switch (SurfaceBlock)
			{
				case E_BLOCK_DIRT:
				case E_BLOCK_GRASS:
				case E_BLOCK_FARMLAND:
				{
					return true;
				}
			}
			return false;
		}
		
		case E_BLOCK_BROWN_MUSHROOM:	// Can be placed on pretty much anything, with exceptions
		case E_BLOCK_RED_MUSHROOM:
		{
			switch (SurfaceBlock)
			{
				case E_BLOCK_GLASS:
				case E_BLOCK_YELLOW_FLOWER:
				case E_BLOCK_RED_ROSE:
				case E_BLOCK_BROWN_MUSHROOM:
				case E_BLOCK_RED_MUSHROOM:
				case E_BLOCK_CACTUS:
				{
					return false;
				}
			}
			return true;
		}
		
		case E_BLOCK_CACTUS:
		{
			if ((SurfaceBlock != E_BLOCK_SAND) && (SurfaceBlock != E_BLOCK_CACTUS))
			{
				// Cactus can only be placed on sand and itself
				return false;
			}

			// Check surroundings. Cacti may ONLY be surrounded by air
			if (
				(GetBlock(a_BlockX - 1, a_BlockY, a_BlockZ)     != E_BLOCK_AIR) ||
				(GetBlock(a_BlockX + 1, a_BlockY, a_BlockZ)     != E_BLOCK_AIR) ||
				(GetBlock(a_BlockX,     a_BlockY, a_BlockZ - 1) != E_BLOCK_AIR) ||
				(GetBlock(a_BlockX,     a_BlockY, a_BlockZ + 1) != E_BLOCK_AIR)
			)
			{
				return false;
			}
			return true;
		}
		
		case E_ITEM_SEEDS:
		case E_ITEM_MELON_SEEDS:
		case E_ITEM_PUMPKIN_SEEDS:
		{
			// Seeds can go only on the farmland block:
			return (SurfaceBlock == E_BLOCK_FARMLAND);
		}
	}  // switch (a_Packet->m_ItemType)
	return true;
}





void cWorld::SetNextBlockTick(int a_BlockX, int a_BlockY, int a_BlockZ)
{
	return m_ChunkMap->SetNextBlockTick(a_BlockX, a_BlockY, a_BlockZ);
}





void cWorld::InitializeSpawn(void)
{
	int ChunkX = 0, ChunkY = 0, ChunkZ = 0;
	BlockToChunk( (int)m_SpawnX, (int)m_SpawnY, (int)m_SpawnZ, ChunkX, ChunkY, ChunkZ );
	
	// For the debugging builds, don't make the server build too much world upon start:
	#ifdef _DEBUG
	int ViewDist = 9;
	#else
	int ViewDist = 20;  // Always prepare an area 20 chunks across, no matter what the actual cClientHandle::VIEWDISTANCE is
	#endif  // _DEBUG
	
	LOG("Preparing spawn area in world \"%s\"...", m_WorldName.c_str());
	for (int x = 0; x < ViewDist; x++)
	{
		for (int z = 0; z < ViewDist; z++)
		{
			m_ChunkMap->TouchChunk( x + ChunkX-(ViewDist - 1) / 2, ZERO_CHUNK_Y, z + ChunkZ-(ViewDist - 1) / 2 );  // Queue the chunk in the generator / loader
		}
	}
	
	{
		// Display progress during this process:
		cWorldLoadProgress Progress(this);
		
		// Wait for the loader to finish loading
		m_Storage.WaitForQueuesEmpty();
		
		// Wait for the generator to finish generating
		m_Generator.WaitForQueueEmpty();
		
		Progress.Stop();
	}
	
	// Light all chunks that have been newly generated:
	LOG("Lighting spawn area in world \"%s\"...", m_WorldName.c_str());
	
	for (int x = 0; x < ViewDist; x++)
	{
		int ChX = x + ChunkX-(ViewDist - 1) / 2;
		for (int z = 0; z < ViewDist; z++)
		{
			int ChZ = z + ChunkZ-(ViewDist - 1) / 2;
			if (!m_ChunkMap->IsChunkLighted(ChX, ChZ))
			{
				m_Lighting.QueueChunk(ChX, ChZ);  // Queue the chunk in the lighting thread
			}
		}  // for z
	}  // for x
	
	{
		cWorldLightingProgress Progress(&m_Lighting);
		m_Lighting.WaitForQueueEmpty();
		Progress.Stop();
	}
	
	// TODO: Better spawn detection - move spawn out of the water if it isn't set in the INI already
	m_SpawnY = (double)GetHeight( (int)m_SpawnX, (int)m_SpawnZ ) + 1.6f; // +1.6f eye height
}





void cWorld::Tick(float a_Dt)
{
	m_Time += a_Dt / 1000.f;

	CurrentTick++;

	bool bSendTime = false;
	m_WorldTimeFraction += a_Dt / 1000.f;
	while ( m_WorldTimeFraction > 1.f )
	{
		m_WorldTimeFraction -= 1.f;
		m_WorldTime += 20;
		bSendTime = true;
	}
	m_WorldTime %= 24000; // 24000 units in a day
	if ( bSendTime )
	{
		Broadcast( cPacket_TimeUpdate( (m_WorldTime) ) );
	}

	{
		cCSLock Lock(m_CSEntities);
		for (cEntityList::iterator itr = m_AllEntities.begin(); itr != m_AllEntities.end();)
		{
			if ((*itr)->IsDestroyed())
			{
				LOG("Destroying entity #%i", (*itr)->GetUniqueID());
				cEntity * RemoveMe = *itr;
				itr = m_AllEntities.erase( itr );
				m_RemoveEntityQueue.push_back( RemoveMe ); 
				continue;
			}
			(*itr)->Tick(a_Dt);
			itr++;
		}
	}

	m_ChunkMap->Tick(a_Dt, m_TickRand);
	
	GetSimulatorManager()->Simulate(a_Dt);

	TickWeather(a_Dt);

	// Asynchronously set blocks:
	sSetBlockList FastSetBlockQueueCopy;
	{
		cCSLock Lock(m_CSFastSetBlock);
		std::swap(FastSetBlockQueueCopy, m_FastSetBlockQueue);
	}
	m_ChunkMap->FastSetBlocks(FastSetBlockQueueCopy);
	if (!FastSetBlockQueueCopy.empty())
	{
		// Some blocks failed, store them for next tick:
		cCSLock Lock(m_CSFastSetBlock);
		m_FastSetBlockQueue.splice(m_FastSetBlockQueue.end(), FastSetBlockQueueCopy);
	}

	if( m_Time - m_LastSave > 60 * 5 ) // Save each 5 minutes
	{
		SaveAllChunks();
	}

	if( m_Time - m_LastUnload > 10 ) // Unload every 10 seconds
	{
		UnloadUnusedChunks();
	}

	// Delete entities queued for removal:
	for (cEntityList::iterator itr = m_RemoveEntityQueue.begin(); itr != m_RemoveEntityQueue.end(); ++itr)
	{
		delete *itr;
	}
	m_RemoveEntityQueue.clear();

	TickSpawnMobs(a_Dt);

	std::vector<int> m_RSList_copy(m_RSList);
	
	m_RSList.clear();

	std::vector<int>::const_iterator cii;	// FIXME - Please rename this variable, WTF is cii??? Use human readable variable names or common abbreviations (i, idx, itr, iter)
	for(cii=m_RSList_copy.begin(); cii!=m_RSList_copy.end();)
	{
		int tempX = *cii;cii++;
		int tempY = *cii;cii++;
		int tempZ = *cii;cii++;
		int state = *cii;cii++;
		
		if ( (state == 11111) && ( (int)GetBlock( tempX, tempY, tempZ ) == E_BLOCK_REDSTONE_TORCH_OFF ) )
		{
			FastSetBlock( tempX, tempY, tempZ, E_BLOCK_REDSTONE_TORCH_ON, (int)GetBlockMeta( tempX, tempY, tempZ ) );
			cRedstone Redstone(this);
			Redstone.ChangeRedstone( tempX, tempY, tempZ, true );
		}
		else if ( (state == 00000) && ( (int)GetBlock( tempX, tempY, tempZ ) == E_BLOCK_REDSTONE_TORCH_ON ) )
		{
			FastSetBlock( tempX, tempY, tempZ, E_BLOCK_REDSTONE_TORCH_OFF, (int)GetBlockMeta( tempX, tempY, tempZ ) );
			cRedstone Redstone(this);
			Redstone.ChangeRedstone( tempX, tempY, tempZ, false );
		}
	}
	m_RSList_copy.erase(m_RSList_copy.begin(),m_RSList_copy.end());
}





void cWorld::TickWeather(float a_Dt)
{
	if ( GetWeather() == 0 )  // if sunny
	{
		if( CurrentTick % 19 == 0 )  //every 20 ticks random weather
		{
			unsigned randWeather = (m_TickRand.randInt() % 10000);
			if (randWeather == 0)
			{
				LOG("Starting Rainstorm!");
				SetWeather ( eWeather_Rain );
			}
			else if  (randWeather == 1)
			{
				LOG("Starting Thunderstorm!");
				SetWeather ( eWeather_ThunderStorm );
			}
		}
	}

	if ( GetWeather() != 0 )  // if raining or thunderstorm
	{
		if ( CurrentTick % 19 == 0 ) // every 20 ticks random weather
		{
			unsigned randWeather = (m_TickRand.randInt() % 4999);
			if (randWeather == 0)  //2% chance per second
			{
				LOG("Back to sunny!");
				SetWeather ( eWeather_Sunny );
			}
			else if ( (randWeather > 4000) && (GetWeather() != 2) )  // random chance for rainstorm to turn into thunderstorm.
			{
				LOG("Starting Thunderstorm!");
				SetWeather ( eWeather_ThunderStorm );
			}
		}
	}

	if ( GetWeather() == 2 )  // if thunderstorm
	{
		if (m_TickRand.randInt() % 199 == 0)  // 0.5% chance per tick of thunderbolt
		{
			CastThunderbolt ( 0, 0, 0 );  // TODO: find random possitions near players to cast thunderbolts.
		}
	}
}





void cWorld::TickSpawnMobs(float a_Dt)
{
	if (!m_bAnimals || (m_Time - m_SpawnMonsterTime <= m_SpawnMonsterRate))
	{
		return;
	}
	
	m_SpawnMonsterTime = m_Time;
	Vector3d SpawnPos;
	{
		cCSLock Lock(m_CSPlayers);
		if ( m_Players.size() <= 0)
		{
			return;
		}
		int RandomPlayerIdx = m_TickRand.randInt() & m_Players.size();
		cPlayerList::iterator itr = m_Players.begin();
		for( int i = 1; i < RandomPlayerIdx; i++ )
		{
			itr++;
		}
		SpawnPos = (*itr)->GetPosition();
	}

	cMonster * Monster = NULL;
	int dayRand   = m_TickRand.randInt() % 6;
	int nightRand = m_TickRand.randInt() % 10;

	SpawnPos += Vector3d( (double)(m_TickRand.randInt() % 64) - 32, (double)(m_TickRand.randInt() % 64) - 32, (double)(m_TickRand.randInt() % 64) - 32 );
	int Height = GetHeight( (int)SpawnPos.x, (int)SpawnPos.z );

	if (m_WorldTime >= 12000 + 1000)
	{
		if (nightRand == 0) //random percent to spawn for night
			Monster = new cSpider();
		else if (nightRand == 1)
			Monster = new cZombie();
		else if (nightRand == 2)
			Monster = new cEnderman();
		else if (nightRand == 3)
			Monster = new cCreeper();
		else if (nightRand == 4)
			Monster = new cCavespider();
		else if (nightRand == 5)
			Monster = new cGhast();
		else if (nightRand == 6)
			Monster = new cZombiepigman();
		else if (nightRand == 7)
			Monster = new cSlime();
		else if (nightRand == 8)
			Monster = new cSilverfish();
		else if (nightRand == 9)
			Monster = new cSkeleton();
		//end random percent to spawn for night
	}
	else
	{
		if (dayRand == 0) //random percent to spawn for day
			Monster = new cChicken();
		else if (dayRand == 1)
			Monster = new cCow();
		else if (dayRand == 2)
			Monster = new cPig();
		else if (dayRand == 3)
			Monster = new cSheep();
		else if (dayRand == 4)
			Monster = new cSquid();
		else if (dayRand == 5)
			Monster = new cWolf();
		//end random percent to spawn for day
	}

	if( Monster )
	{
		Monster->Initialize( this );
		Monster->TeleportTo( SpawnPos.x, (double)(Height) + 2, SpawnPos.z );
		Monster->SpawnOn(0);
	}
}





void cWorld::GrowTree( int a_X, int a_Y, int a_Z )
{
	if (GetBlock(a_X, a_Y, a_Z) == E_BLOCK_SAPLING)
	{
		// There is a sapling here, grow a tree according to its type:
		GrowTreeFromSapling(a_X, a_Y, a_Z, GetBlockMeta(a_X, a_Y, a_Z));
	}
	else
	{
		// There is nothing here, grow a tree based on the current biome here:
		GrowTreeByBiome(a_X, a_Y, a_Z);
	}
}





void cWorld::GrowTreeFromSapling(int a_X, int a_Y, int a_Z, char a_SaplingMeta)
{
	cNoise Noise(m_Generator.GetSeed());
	sSetBlockVector Blocks;
	switch (a_SaplingMeta & 0x07)
	{
		case E_META_SAPLING_APPLE:   GetAppleTreeImage  (a_X, a_Y, a_Z, Noise, (int)(m_WorldTime & 0xffffffff), Blocks); break;
		case E_META_SAPLING_BIRCH:   GetBirchTreeImage  (a_X, a_Y, a_Z, Noise, (int)(m_WorldTime & 0xffffffff), Blocks); break;
		case E_META_SAPLING_CONIFER: GetConiferTreeImage(a_X, a_Y, a_Z, Noise, (int)(m_WorldTime & 0xffffffff), Blocks); break;
		case E_META_SAPLING_JUNGLE:  GetJungleTreeImage (a_X, a_Y, a_Z, Noise, (int)(m_WorldTime & 0xffffffff), Blocks); break;
	}
	
	GrowTreeImage(Blocks);
}





void cWorld::GrowTreeByBiome(int a_X, int a_Y, int a_Z)
{
	cNoise Noise(m_Generator.GetSeed());
	sSetBlockVector Blocks;
	GetTreeImageByBiome(a_X, a_Y, a_Z, Noise, (int)(m_WorldTime & 0xffffffff), (EMCSBiome)GetBiomeAt(a_X, a_Z), Blocks);
	GrowTreeImage(Blocks);
}





void cWorld::GrowTreeImage(const sSetBlockVector & a_Blocks)
{
	// Check that the tree has place to grow
	
	// Make a copy of the log blocks:
	sSetBlockVector b2;
	for (sSetBlockVector::const_iterator itr = a_Blocks.begin(); itr != a_Blocks.end(); ++itr)
	{
		if (itr->BlockType == E_BLOCK_LOG)
		{
			b2.push_back(*itr);
		}
	}  // for itr - a_Blocks[]
	
	// Query blocktypes and metas at those log blocks:
	if (!GetBlocks(b2, false))
	{
		return;
	}
	
	// Check that at each log's coord there's an block allowed to be overwritten:
	for (sSetBlockVector::const_iterator itr = b2.begin(); itr != b2.end(); ++itr)
	{
		switch (itr->BlockType)
		{
			CASE_TREE_ALLOWED_BLOCKS:
			{
				break;
			}
			default:
			{
				return;
			}
		}
	}  // for itr - b2[]
	
	// All ok, replace blocks with the tree image:
	m_ChunkMap->ReplaceTreeBlocks(a_Blocks);
}





bool cWorld::GrowPlant(int a_BlockX, int a_BlockY, int a_BlockZ, bool a_IsByBonemeal)
{
	BLOCKTYPE BlockType;
	NIBBLETYPE BlockMeta;
	GetBlockTypeMeta(a_BlockX, a_BlockY, a_BlockZ, BlockType, BlockMeta);
	switch (BlockType)
	{
		case E_BLOCK_CROPS:
		{
			if (a_IsByBonemeal && !m_IsGrassBonemealable)
			{
				return false;
			}
			if (BlockMeta < 7)
			{
				FastSetBlock(a_BlockX, a_BlockY, a_BlockZ, BlockType, 7);
			}
			return true;
		}
		
		case E_BLOCK_MELON_STEM:
		{
			if (BlockMeta < 7)
			{
				if (a_IsByBonemeal && !m_IsMelonStemBonemealable)
				{
					return false;
				}
				FastSetBlock(a_BlockX, a_BlockY, a_BlockZ, BlockType, 7);
			}
			else
			{
				if (a_IsByBonemeal && !m_IsMelonBonemealable)
				{
					return false;
				}
				GrowMelonPumpkin(a_BlockX, a_BlockY, a_BlockZ, BlockType);
			}
			return true;
		}
		
		case E_BLOCK_PUMPKIN_STEM:
		{
			if (BlockMeta < 7)
			{
				if (a_IsByBonemeal && !m_IsPumpkinStemBonemealable)
				{
					return false;
				}
				FastSetBlock(a_BlockX, a_BlockY, a_BlockZ, BlockType, 7);
			}
			else
			{
				if (a_IsByBonemeal && !m_IsPumpkinBonemealable)
				{
					return false;
				}
				GrowMelonPumpkin(a_BlockX, a_BlockY, a_BlockZ, BlockType);
			}
			return true;
		}
		
		case E_BLOCK_SAPLING:
		{
			if (a_IsByBonemeal && !m_IsSaplingBonemealable)
			{
				return false;
			}
			GrowTreeFromSapling(a_BlockX, a_BlockY, a_BlockZ, BlockMeta);
			return true;
		}
		
		case E_BLOCK_GRASS:
		{
			if (a_IsByBonemeal && !m_IsGrassBonemealable)
			{
				return false;
			}
			MTRand r1;
			for (int i = 0; i < 60; i++)
			{
				int OfsX = (r1.randInt(3) + r1.randInt(3) + r1.randInt(3) + r1.randInt(3)) / 2 - 3;
				int OfsY = r1.randInt(3) + r1.randInt(3) - 3;
				int OfsZ = (r1.randInt(3) + r1.randInt(3) + r1.randInt(3) + r1.randInt(3)) / 2 - 3;
				BLOCKTYPE Ground = GetBlock(a_BlockX + OfsX, a_BlockY + OfsY, a_BlockZ + OfsZ);
				if (Ground != E_BLOCK_GRASS)
				{
					continue;
				}
				BLOCKTYPE Above  = GetBlock(a_BlockX + OfsX, a_BlockY + OfsY + 1, a_BlockZ + OfsZ);
				if (Above != E_BLOCK_AIR)
				{
					continue;
				}
				BLOCKTYPE  SpawnType;
				NIBBLETYPE SpawnMeta = 0;
				switch (r1.randInt(10))
				{
					case 0:  SpawnType = E_BLOCK_YELLOW_FLOWER; break;
					case 1:  SpawnType = E_BLOCK_RED_ROSE;      break;
					default:
					{
						SpawnType = E_BLOCK_TALL_GRASS;
						SpawnMeta = E_META_TALL_GRASS_GRASS;
						break;
					}
				}  // switch (random spawn block)
				FastSetBlock(a_BlockX + OfsX, a_BlockY + OfsY + 1, a_BlockZ + OfsZ, SpawnType, SpawnMeta);
			}  // for i - 50 times
			return true;
		}
		
		case E_BLOCK_SUGARCANE:
		{
			if (a_IsByBonemeal && !m_IsSugarcaneBonemealable)
			{
				return false;
			}
			m_ChunkMap->GrowSugarcane(a_BlockX, a_BlockY, a_BlockZ, 3);
			return true;
		}
		
		case E_BLOCK_CACTUS:
		{
			if (a_IsByBonemeal && !m_IsCactusBonemealable)
			{
				return false;
			}
			m_ChunkMap->GrowCactus(a_BlockX, a_BlockY, a_BlockZ, 3);
			return true;
		}
	}  // switch (BlockType)
	return false;
}





void cWorld::GrowMelonPumpkin(int a_BlockX, int a_BlockY, int a_BlockZ, char a_BlockType)
{
	MTRand Rand;
	m_ChunkMap->GrowMelonPumpkin(a_BlockX, a_BlockY, a_BlockZ, a_BlockType, Rand);
}





int cWorld::GetBiomeAt (int a_BlockX, int a_BlockZ)
{
	return m_ChunkMap->GetBiomeAt(a_BlockX, a_BlockZ);
}





void cWorld::SetBlock( int a_X, int a_Y, int a_Z, char a_BlockType, char a_BlockMeta )
{
	m_ChunkMap->SetBlock(a_X, a_Y, a_Z, a_BlockType, a_BlockMeta);

	GetSimulatorManager()->WakeUp(a_X, a_Y, a_Z);
}





void cWorld::FastSetBlock( int a_X, int a_Y, int a_Z, char a_BlockType, char a_BlockMeta )
{
	cCSLock Lock(m_CSFastSetBlock);
	m_FastSetBlockQueue.push_back(sSetBlock(a_X, a_Y, a_Z, a_BlockType, a_BlockMeta)); 
}





char cWorld::GetBlock(int a_X, int a_Y, int a_Z)
{
	// First check if it isn't queued in the m_FastSetBlockQueue:
	{
		int X = a_X, Y = a_Y, Z = a_Z;
		int ChunkX, ChunkY, ChunkZ;
		AbsoluteToRelative(X, Y, Z, ChunkX, ChunkY, ChunkZ);
		
		cCSLock Lock(m_CSFastSetBlock);
		for (sSetBlockList::iterator itr = m_FastSetBlockQueue.begin(); itr != m_FastSetBlockQueue.end(); ++itr)
		{
			if ((itr->x == X) && (itr->y == Y) && (itr->z == Z) && (itr->ChunkX == ChunkX) && (itr->ChunkZ == ChunkZ))
			{
				return itr->BlockType;
			}
		}  // for itr - m_FastSetBlockQueue[]
	}
	
	return m_ChunkMap->GetBlock(a_X, a_Y, a_Z);
}





char cWorld::GetBlockMeta( int a_X, int a_Y, int a_Z )
{
	// First check if it isn't queued in the m_FastSetBlockQueue:
	{
		cCSLock Lock(m_CSFastSetBlock);
		for (sSetBlockList::iterator itr = m_FastSetBlockQueue.begin(); itr != m_FastSetBlockQueue.end(); ++itr)
		{
			if ((itr->x == a_X) && (itr->y == a_Y) && (itr->y == a_Y))
			{
				return itr->BlockMeta;
			}
		}  // for itr - m_FastSetBlockQueue[]
	}
	
	return m_ChunkMap->GetBlockMeta(a_X, a_Y, a_Z);
}





void cWorld::SetBlockMeta( int a_X, int a_Y, int a_Z, char a_MetaData )
{
	m_ChunkMap->SetBlockMeta(a_X, a_Y, a_Z, a_MetaData);
}





char cWorld::GetBlockSkyLight( int a_X, int a_Y, int a_Z )
{
	return m_ChunkMap->GetBlockSkyLight(a_X, a_Y, a_Z);
}





void cWorld::GetBlockTypeMeta(int a_BlockX, int a_BlockY, int a_BlockZ, char & a_BlockType, unsigned char & a_BlockMeta)
{
	m_ChunkMap->GetBlockTypeMeta(a_BlockX, a_BlockY, a_BlockZ, (BLOCKTYPE &)a_BlockType, (NIBBLETYPE &)a_BlockMeta);
}





void cWorld::SpawnItemPickups(const cItems & a_Pickups, double a_BlockX, double a_BlockY, double a_BlockZ, double a_FlyAwaySpeed)
{
	MTRand r1;
	a_FlyAwaySpeed /= 1000;  // Pre-divide, so that we can don't have to divide each time inside the loop
	for (cItems::const_iterator itr = a_Pickups.begin(); itr != a_Pickups.end(); ++itr)
	{
		float SpeedX = (float)(a_FlyAwaySpeed * (r1.randInt(1000) - 500));
		float SpeedY = (float)(a_FlyAwaySpeed *  r1.randInt(1000));
		float SpeedZ = (float)(a_FlyAwaySpeed * (r1.randInt(1000) - 500));
		cPickup * Pickup = new cPickup(
			(int)(a_BlockX * 32) + r1.randInt(16) + r1.randInt(16), 
			(int)(a_BlockY * 32) + r1.randInt(16) + r1.randInt(16), 
			(int)(a_BlockZ * 32) + r1.randInt(16) + r1.randInt(16),
			*itr, SpeedX, SpeedY, SpeedZ
		);
		Pickup->Initialize(this);
	}
}





void cWorld::SpawnItemPickups(const cItems & a_Pickups, double a_BlockX, double a_BlockY, double a_BlockZ, double a_SpeedX, double a_SpeedY, double a_SpeedZ)
{
	MTRand r1;
	for (cItems::const_iterator itr = a_Pickups.begin(); itr != a_Pickups.end(); ++itr)
	{
		cPickup * Pickup = new cPickup(
			(int)(a_BlockX * 32) + r1.randInt(16) + r1.randInt(16), 
			(int)(a_BlockY * 32) + r1.randInt(16) + r1.randInt(16), 
			(int)(a_BlockZ * 32) + r1.randInt(16) + r1.randInt(16),
			*itr, (float)a_SpeedX, (float)a_SpeedY, (float)a_SpeedZ
		);
		Pickup->Initialize(this);
	}
}





void cWorld::ReplaceBlocks(const sSetBlockVector & a_Blocks, BLOCKTYPE a_FilterBlockType)
{
	m_ChunkMap->ReplaceBlocks(a_Blocks, a_FilterBlockType);
}





bool cWorld::GetBlocks(sSetBlockVector & a_Blocks, bool a_ContinueOnFailure)
{
	return m_ChunkMap->GetBlocks(a_Blocks, a_ContinueOnFailure);
}





bool cWorld::DigBlock( int a_X, int a_Y, int a_Z)
{
	return m_ChunkMap->DigBlock(a_X, a_Y, a_Z);
}





void cWorld::SendBlockTo( int a_X, int a_Y, int a_Z, cPlayer * a_Player )
{
	m_ChunkMap->SendBlockTo(a_X, a_Y, a_Z, a_Player);
}





// TODO: This interface is dangerous!
cBlockEntity * cWorld::GetBlockEntity( int a_X, int a_Y, int a_Z )
{
	return NULL;
}





int cWorld::GetHeight( int a_X, int a_Z )
{
	return m_ChunkMap->GetHeight(a_X, a_Z);
}





const double & cWorld::GetSpawnY(void)
{
	return m_SpawnY;
}




void cWorld::Broadcast( const cPacket & a_Packet, cClientHandle * a_Exclude)
{
	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		cClientHandle * ch = (*itr)->GetClientHandle();
		if ((ch == a_Exclude) || (ch == NULL) || !ch->IsLoggedIn() || ch->IsDestroyed())
		{
			continue;
		}
		(*itr)->GetClientHandle()->Send( a_Packet );
	}
}





void cWorld::BroadcastToChunk(int a_ChunkX, int a_ChunkY, int a_ChunkZ, const cPacket & a_Packet, cClientHandle * a_Exclude)
{
	m_ChunkMap->BroadcastToChunk(a_ChunkX, a_ChunkY, a_ChunkZ, a_Packet, a_Exclude);
}





void cWorld::BroadcastToChunkOfBlock(int a_X, int a_Y, int a_Z, cPacket * a_Packet, cClientHandle * a_Exclude)
{
	m_ChunkMap->BroadcastToChunkOfBlock(a_X, a_Y, a_Z, a_Packet, a_Exclude);
}





void cWorld::MarkChunkDirty (int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkDirty (a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::MarkChunkSaving(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkSaving(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::MarkChunkSaved (int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkSaved (a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::SetChunkData(
	int a_ChunkX, int a_ChunkY, int a_ChunkZ,
	const BLOCKTYPE * a_BlockTypes,
	const NIBBLETYPE * a_BlockMeta,
	const NIBBLETYPE * a_BlockLight,
	const NIBBLETYPE * a_BlockSkyLight,
	const cChunkDef::HeightMap * a_HeightMap,
	const cChunkDef::BiomeMap  * a_BiomeMap,
	cEntityList & a_Entities, 
	cBlockEntityList & a_BlockEntities,
	bool a_MarkDirty
)
{
	// Validate biomes, if needed:
	cChunkDef::BiomeMap BiomeMap;
	const cChunkDef::BiomeMap * Biomes = a_BiomeMap;
	if (a_BiomeMap == NULL)
	{
		// The biomes are not assigned, get them from the generator:
		Biomes = &BiomeMap;
		m_Generator.GenerateBiomes(a_ChunkX, a_ChunkZ, BiomeMap);
	}
	
	m_ChunkMap->SetChunkData(
		a_ChunkX, a_ChunkY, a_ChunkZ, 
		a_BlockTypes, a_BlockMeta, a_BlockLight, a_BlockSkyLight,
		a_HeightMap, *Biomes,
		a_Entities, a_BlockEntities,
		a_MarkDirty
	);
	
	// If a client is requesting this chunk, send it to them:
	if (m_ChunkMap->HasChunkAnyClients(a_ChunkX, a_ChunkY, a_ChunkZ))
	{
		m_ChunkSender.ChunkReady(a_ChunkX, a_ChunkY, a_ChunkZ);
	}
	
	// Notify the lighting thread that the chunk has become valid (in case it is a neighbor of a postponed chunk):
	m_Lighting.ChunkReady(a_ChunkX, a_ChunkZ);
}





void cWorld::ChunkLighted(
	int a_ChunkX, int a_ChunkZ,
	const cChunkDef::BlockNibbles & a_BlockLight,
	const cChunkDef::BlockNibbles & a_SkyLight
)
{
	m_ChunkMap->ChunkLighted(a_ChunkX, a_ChunkZ, a_BlockLight, a_SkyLight);
}





bool cWorld::GetChunkData(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cChunkDataCallback & a_Callback)
{
	return m_ChunkMap->GetChunkData(a_ChunkX, a_ChunkY, a_ChunkZ, a_Callback);
}





bool cWorld::GetChunkBlockTypes(int a_ChunkX, int a_ChunkY, int a_ChunkZ, BLOCKTYPE * a_BlockTypes)
{
	return m_ChunkMap->GetChunkBlockTypes(a_ChunkX, a_ChunkY, a_ChunkZ, a_BlockTypes);
}





bool cWorld::GetChunkBlockData(int a_ChunkX, int a_ChunkY, int a_ChunkZ, BLOCKTYPE * a_BlockData)
{
	return m_ChunkMap->GetChunkBlockData(a_ChunkX, a_ChunkY, a_ChunkZ, a_BlockData);
}





bool cWorld::IsChunkValid(int a_ChunkX, int a_ChunkY, int a_ChunkZ) const
{
	return m_ChunkMap->IsChunkValid(a_ChunkX, a_ChunkY, a_ChunkZ);
}





bool cWorld::HasChunkAnyClients(int a_ChunkX, int a_ChunkY, int a_ChunkZ) const
{
	return m_ChunkMap->HasChunkAnyClients(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::UnloadUnusedChunks(void )
{
	m_LastUnload = m_Time;
	m_ChunkMap->UnloadUnusedChunks();
}





void cWorld::CollectPickupsByPlayer(cPlayer * a_Player)
{
	m_ChunkMap->CollectPickupsByPlayer(a_Player);
}





void cWorld::SetMaxPlayers(int iMax)
{
	m_MaxPlayers = MAX_PLAYERS;
	if (iMax > 0 && iMax < MAX_PLAYERS)
	{
		m_MaxPlayers = iMax;
	}
}





void cWorld::AddPlayer( cPlayer* a_Player )
{
	cCSLock Lock(m_CSPlayers);
	
	ASSERT(std::find(m_Players.begin(), m_Players.end(), a_Player) == m_Players.end());  // Is it already in the list? HOW?
	
	m_Players.remove( a_Player );  // Make sure the player is registered only once
	m_Players.push_back( a_Player );
}





void cWorld::RemovePlayer( cPlayer* a_Player )
{
	cCSLock Lock(m_CSPlayers);
	m_Players.remove( a_Player );
}





bool cWorld::ForEachPlayer(cPlayerListCallback & a_Callback)
{
	// Calls the callback for each player in the list
	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		if (a_Callback.Item(*itr))
		{
			return false;
		}
	}  // for itr - m_Players[]
	return true;
}




// TODO: This interface is dangerous!
cPlayer* cWorld::GetPlayer( const char* a_PlayerName )
{
	cPlayer* BestMatch = 0;
	unsigned int MatchedLetters = 0;
	unsigned int NumMatches = 0;
	bool bPerfectMatch = false;

	unsigned int NameLength = strlen( a_PlayerName );
	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); itr++ )
	{
		std::string Name = (*itr)->GetName();
		if( NameLength > Name.length() ) continue; // Definitely not a match

		for (unsigned int i = 0; i < NameLength; i++)
		{
			char c1 = (char)toupper( a_PlayerName[i] );
			char c2 = (char)toupper( Name[i] );
			if( c1 == c2 )
			{
				if( i+1 > MatchedLetters )
				{
					MatchedLetters = i+1;
					BestMatch = *itr;
				}
				if( i+1 == NameLength )
				{
					NumMatches++;
					if( NameLength == Name.length() )
					{
						bPerfectMatch = true;
						break;
					}
				}
			}
			else
			{
				if( BestMatch == *itr ) BestMatch = 0;
				break;
			}
			if( bPerfectMatch )
				break;
		}
	}
	if ( NumMatches == 1 )
	{
		return BestMatch;
	}

	// More than one matches, so it's undefined. Return NULL instead
	return NULL;
}





cPlayer * cWorld::FindClosestPlayer(const Vector3f & a_Pos, float a_SightLimit)
{
	cTracer LineOfSight(this);

	float ClosestDistance = a_SightLimit;
	cPlayer* ClosestPlayer = NULL;

	cCSLock Lock(m_CSPlayers);
	for (cPlayerList::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		Vector3f Pos = (*itr)->GetPosition();
		float Distance = (Pos - a_Pos).Length();

		if (Distance <= a_SightLimit)
		{
			if (!LineOfSight.Trace(a_Pos,(Pos - a_Pos),(int)(Pos - a_Pos).Length()))
			{
				if (Distance < ClosestDistance)
				{
					ClosestDistance = Distance;
					ClosestPlayer = *itr;
				}
			}
		}
	}
	return ClosestPlayer;
}





void cWorld::SendPlayerList(cPlayer * a_DestPlayer)
{
	// Sends the playerlist to a_DestPlayer
	cCSLock Lock(m_CSPlayers);
	for ( cPlayerList::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
	{
		cClientHandle * ch = (*itr)->GetClientHandle();
		if ((ch != NULL) && !ch->IsDestroyed())
		{
			cPacket_PlayerListItem PlayerListItem((*itr)->GetColor() + (*itr)->GetName(), true, (*itr)->GetClientHandle()->GetPing());
			a_DestPlayer->GetClientHandle()->Send( PlayerListItem );
		}
	}
}





bool cWorld::DoWithEntity( int a_UniqueID, cEntityCallback & a_Callback )
{
	cCSLock Lock(m_CSEntities);
	for (cEntityList::iterator itr = m_AllEntities.begin(); itr != m_AllEntities.end(); ++itr )
	{
		if( (*itr)->GetUniqueID() == a_UniqueID )
		{
			return a_Callback.Item(*itr);
		}
	} // for itr - m_AllEntities[]
	return false;
}





void cWorld::RemoveEntityFromChunk(cEntity * a_Entity, int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->RemoveEntityFromChunk(a_Entity, a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::MoveEntityToChunk(cEntity * a_Entity, int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->MoveEntityToChunk(a_Entity, a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::CompareChunkClients(int a_ChunkX1, int a_ChunkY1, int a_ChunkZ1, int a_ChunkX2, int a_ChunkY2, int a_ChunkZ2, cClientDiffCallback & a_Callback)
{
	m_ChunkMap->CompareChunkClients(a_ChunkX1, a_ChunkY1, a_ChunkZ1, a_ChunkX2, a_ChunkY2, a_ChunkZ2, a_Callback);
}





bool cWorld::AddChunkClient(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cClientHandle * a_Client)
{
	return m_ChunkMap->AddChunkClient(a_ChunkX, a_ChunkY, a_ChunkZ, a_Client);
}





void cWorld::RemoveChunkClient(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cClientHandle * a_Client)
{
	m_ChunkMap->RemoveChunkClient(a_ChunkX, a_ChunkY, a_ChunkZ, a_Client);
}





void cWorld::RemoveClientFromChunks(cClientHandle * a_Client)
{
	m_ChunkMap->RemoveClientFromChunks(a_Client);
}





void cWorld::SendChunkTo(int a_ChunkX, int a_ChunkY, int a_ChunkZ, cClientHandle * a_Client)
{
	m_ChunkSender.QueueSendChunkTo(a_ChunkX, a_ChunkY, a_ChunkZ, a_Client);
}





void cWorld::RemoveClientFromChunkSender(cClientHandle * a_Client)
{
	m_ChunkSender.RemoveClient(a_Client);
}





void cWorld::TouchChunk(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->TouchChunk(a_ChunkX, a_ChunkY, a_ChunkZ);
}





bool cWorld::LoadChunk(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	return m_ChunkMap->LoadChunk(a_ChunkX, a_ChunkY, a_ChunkZ);
}



	

void cWorld::LoadChunks(const cChunkCoordsList & a_Chunks)
{
	m_ChunkMap->LoadChunks(a_Chunks);
}





void cWorld::ChunkLoadFailed(int a_ChunkX, int a_ChunkY, int a_ChunkZ)
{
	m_ChunkMap->ChunkLoadFailed(a_ChunkX, a_ChunkY, a_ChunkZ);
}





void cWorld::UpdateSign(int a_X, int a_Y, int a_Z, const AString & a_Line1, const AString & a_Line2, const AString & a_Line3, const AString & a_Line4)
{
	m_ChunkMap->UpdateSign(a_X, a_Y, a_Z, a_Line1, a_Line2, a_Line3, a_Line4);
}





void cWorld::ChunksStay(const cChunkCoordsList & a_Chunks, bool a_Stay)
{
	m_ChunkMap->ChunksStay(a_Chunks, a_Stay);
}





void cWorld::RegenerateChunk(int a_ChunkX, int a_ChunkZ)
{
	m_ChunkMap->MarkChunkRegenerating(a_ChunkX, a_ChunkZ);
	
	// Trick: use Y=1 to force the chunk generation even though the chunk data is already present
	m_Generator.QueueGenerateChunk(a_ChunkX, 1, a_ChunkZ);
}





void cWorld::GenerateChunk(int a_ChunkX, int a_ChunkZ)
{
	m_Generator.QueueGenerateChunk(a_ChunkX, ZERO_CHUNK_Y, a_ChunkZ);
}





void cWorld::QueueLightChunk(int a_ChunkX, int a_ChunkZ, cChunkCoordCallback * a_Callback)
{
	m_Lighting.QueueChunk(a_ChunkX, a_ChunkZ, a_Callback);
}





bool cWorld::IsChunkLighted(int a_ChunkX, int a_ChunkZ)
{
	return m_ChunkMap->IsChunkLighted(a_ChunkX, a_ChunkZ);
}





void cWorld::SaveAllChunks(void)
{
	LOG("Saving all chunks...");
	m_LastSave = m_Time;
	m_ChunkMap->SaveAllChunks();
}





/************************************************************************/
/* Get and set                                                          */
/************************************************************************/
// void cWorld::AddClient( cClientHandle* a_Client )
// {
// 	m_m_Clients.push_back( a_Client );
// }
// cWorld::ClientList & cWorld::GetClients()
// {
// 	return m_m_Clients;
// }





void cWorld::AddEntity( cEntity* a_Entity )
{
	cCSLock Lock(m_CSEntities);
	m_AllEntities.push_back( a_Entity ); 
}





unsigned int cWorld::GetNumPlayers()
{
	cCSLock Lock(m_CSPlayers);
	return m_Players.size(); 
}





int cWorld::GetNumChunks(void) const
{
	return m_ChunkMap->GetNumChunks();
}





void cWorld::GetChunkStats(int & a_NumValid, int & a_NumDirty, int & a_NumInLightingQueue)
{
	m_ChunkMap->GetChunkStats(a_NumValid, a_NumDirty);
	a_NumInLightingQueue = (int) m_Lighting.GetQueueLength();
}




