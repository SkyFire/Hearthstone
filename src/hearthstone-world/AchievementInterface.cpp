#include "StdAfx.h"

AchievementInterface::AchievementInterface(Player& plr) : m_player(plr)
{
}

AchievementInterface::~AchievementInterface()
{
	if( m_achivementDataMap.size() )
	{
		std::map<uint32,AchievementData*>::iterator itr = m_achivementDataMap.begin();
		for(; itr != m_achivementDataMap.end(); ++itr)
		{
			delete itr->second;
		}
	}
}

void AchievementInterface::LoadFromDB( QueryResult * pResult )
{
	if( !pResult ) // We have no achievements yet. :)
		return;

	do 
	{
		Field * fields = pResult->Fetch();
		uint32 achievementid = fields[2].GetUInt32();
		string criteriaprogress = fields[3].GetString();
		bool completed = (fields[4].GetUInt32() > 0);

		AchievementEntry * ae = dbcAchievement.LookupEntry( achievementid );
		AchievementData * ad = new AchievementData;
		memset(ad, 0, sizeof(AchievementData));

		ad->id = achievementid;
		ad->num_criterias = ae->AssociatedCriteriaCount;
		ad->completed = completed;
		ad->date = fields[4].GetUInt32();
		

		vector<string> Delim = StrSplit( criteriaprogress, "," );
		for( uint32 i = 0; i < 32; ++i)
		{
			if( i >= Delim.size() )
				continue;

			string posValue = Delim[i];
			if( !posValue.size() )
				continue;

			uint32 r = atoi(posValue.c_str());
			ad->counter[i] = r;

			//printf("Loaded achievement: %u, %s\n", ae->ID, ad->completed ? "completed" : "incomplete" );
		}

		m_achivementDataMap.insert( make_pair( achievementid, ad) );
	} while ( pResult->NextRow() );
}

void AchievementInterface::SaveToDB(QueryBuffer * buffer)
{
	bool NewBuffer = false;
	if( !buffer )
	{
		buffer = new QueryBuffer;
		NewBuffer = true;
	}

	map<uint32,AchievementData*>::iterator itr = m_achivementDataMap.begin();
	for(; itr != m_achivementDataMap.end(); ++itr)
	{
		AchievementData * ad = itr->second;
		std::stringstream ss;
		ss << "REPLACE INTO achievements (player,achievementid,progress,completed) VALUES (";
		ss << m_player.GetLowGUID() << ",";
		ss << ad->id << ",";
		ss << "'";
		for(uint32 i = 0; i < ad->num_criterias; ++i)
		{
			ss << ad->counter[i] << ",";
		}
		ss << "',";
		ss << ad->date << ")";
		buffer->AddQueryStr( ss.str().c_str() );
	}

	if( NewBuffer )
		CharacterDatabase.AddQueryBuffer( buffer );
}

WorldPacket* AchievementInterface::BuildAchievementData()
{
	WorldPacket * data = new WorldPacket(SMSG_ALL_ACHIEVEMENT_DATA, 400);
	std::map<uint32,AchievementData*>::iterator itr = m_achivementDataMap.begin();
	for(; itr != m_achivementDataMap.end(); ++itr)
	{
		if( itr->second->completed )
		{
			*data << uint32(itr->first);
			*data << uint32( unixTimeToTimeBitfields(itr->second->date) );
		}
	}

	*data << int32(-1);
	itr = m_achivementDataMap.begin(); // Re-loop, luls
	for(; itr != m_achivementDataMap.end(); ++itr)
	{
		if( !itr->second->completed )
		{
			AchievementEntry * ae = dbcAchievement.LookupEntry( itr->second->id );
			// Loop over the associated criteria
			for(uint32 i = 0; i < ae->AssociatedCriteriaCount; ++i)
			{
				*data << uint32( ae->AssociatedCriteria[i] );
				uint32 counterVar = itr->second->counter[i];
				data->AppendPacked64( (uint64)counterVar );
				data->append(m_player.GetNewGUID());
				*data << uint32(0);
				*data << uint32( unixTimeToTimeBitfields( time(NULL) ) );
				*data << uint32(0);
				*data << uint32(0);
			}
		}
	}
	*data << int32(-1);
	return data;
}

void AchievementInterface::EventAchievementEarned(AchievementData * pData)
{
	pData->completed = true;
	pData->date = (uint32)time(NULL);

	m_player.SendMessageToSet( BuildAchievementEarned(pData), true );

	HandleAchievementCriteriaRequiresAchievement(pData->id);
}



WorldPacket* AchievementInterface::BuildAchievementEarned(AchievementData * pData)
{
	WorldPacket * data = new WorldPacket(SMSG_ACHIEVEMENT_EARNED, 40);
	*data << m_player.GetNewGUID();
	*data << pData->id;
	*data << uint32( unixTimeToTimeBitfields(time(NULL)) );
	*data << uint32(0);
	return data;
}

AchievementData* AchievementInterface::CreateAchievementDataEntryForAchievement(AchievementEntry * ae)
{
	AchievementData * ad = new AchievementData;
	memset(ad, 0, sizeof(AchievementData));
	ad->id = ae->ID;
	ad->num_criterias = ae->AssociatedCriteriaCount;
	m_achivementDataMap.insert( make_pair( ad->id, ad ) );
	return ad;
}

bool AchievementInterface::CanCompleteAchievement(AchievementData * ad)
{
	if( ad->completed ) return false;

	bool hasCompleted = false;
	AchievementEntry * ach = dbcAchievement.LookupEntry(ad->id);
	if( ach->is_statistic == 1 ) // We cannot complete statistics
		return false;

	bool failedOne = false;
	for(uint32 i = 0; i < ad->num_criterias; ++i)
	{
		bool thisFail = false;
		AchievementCriteriaEntry * ace = dbcAchivementCriteria.LookupEntry(ach->AssociatedCriteria[i]);
		uint32 ReqCount = ace->raw.field4 ? ace->raw.field4 : 1;

		if( ace->groupFlag & ACHIEVEMENT_CRITERIA_GROUP_NOT_IN_GROUP && m_player.GetGroup() )
			return false;

		if( ace->timeLimit && ace->timeLimit < ad->completionTimeLast )
			thisFail = true;

		if( ad->counter[i] < ReqCount )
			thisFail = true;

		if( ach->factionFlag == ACHIEVEMENT_FACTION_ALLIANCE && m_player.GetTeam() == 1 )
			thisFail = true;

		if( ach->factionFlag == ACHIEVEMENT_FACTION_HORDE && m_player.GetTeam() == 0 )
			thisFail = true;

		if( thisFail && ace->completionFlag & ACHIEVEMENT_CRITERIA_COMPLETE_ONE_FLAG )
			failedOne = true;
		else if(thisFail)
			return false;

		if( !thisFail && ace->completionFlag & ACHIEVEMENT_CRITERIA_COMPLETE_ONE_FLAG )
			hasCompleted = true;
	}

	if( failedOne && !hasCompleted )
		return false;

	return true;
}

AchievementData* AchievementInterface::GetAchievementDataByAchievementID(uint32 ID)
{
	map<uint32,AchievementData*>::iterator itr = m_achivementDataMap.find( ID );
	if( itr != m_achivementDataMap.end() )
		return itr->second;
	else
	{
		AchievementEntry * ae = dbcAchievement.LookupEntryForced(ID);
		if(!ae)
		{
			printf("No achievement for %u!\n", ID);
		}
		return CreateAchievementDataEntryForAchievement(ae);
	}
}

void AchievementInterface::SendCriteriaUpdate(AchievementData * ad, uint32 idx)
{
	ad->date = (uint32)time(NULL);
	WorldPacket data(SMSG_CRITERIA_UPDATE, 50);
	AchievementEntry * ae = dbcAchievement.LookupEntry(ad->id);
	data << uint32(ae->AssociatedCriteria[idx]);

	data.AppendPacked64((uint64)ad->counter[idx]);

	data << m_player.GetNewGUID();   
	data << uint32(0);
	data << uint32(unixTimeToTimeBitfields(time(NULL)));
	data << uint32(0);
	data << uint32(0); 

	if( !m_player.IsInWorld() )
		m_player.CopyAndSendDelayedPacket(&data);
	else
		m_player.GetSession()->SendPacket(&data);
}

void AchievementInterface::HandleAchievementCriteriaConditionDeath()
{
	// We died, so reset all our achievements that have ACHIEVEMENT_CRITERIA_CONDITION_NO_DEATH
	if( !m_achivementDataMap.size() )
		return;

	map<uint32,AchievementData*>::iterator itr = m_achivementDataMap.begin();
	for(; itr != m_achivementDataMap.end(); ++itr)
	{
		AchievementData * ad = itr->second;
		if(ad->completed) continue;
		AchievementEntry * ae = dbcAchievement.LookupEntry( ad->id );
		for(uint32 i = 0; i < ad->num_criterias; ++i)
		{
			uint32 CriteriaID = ae->AssociatedCriteria[i];
			AchievementCriteriaEntry * ace = dbcAchivementCriteria.LookupEntry( CriteriaID );
			if( ad->counter[i] && ace->raw.additionalRequirement1_type & ACHIEVEMENT_CRITERIA_CONDITION_NO_DEATH )
			{
				ad->counter[i] = 0;
				SendCriteriaUpdate(ad, i);
			}
		}
	}
}

void AchievementInterface::HandleAchievementCriteriaKillCreature(uint32 killedMonster)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_KILL_CREATURE );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
	{
		return;
	}
	
	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqKill = ace->kill_creature.creatureID;
		uint32 ReqCount = ace->kill_creature.creatureCount;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;


		// Wrong monster, continue on, kids.
		if( ReqKill != killedMonster )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaWinBattleground(uint32 bgMapId, uint32 scoreMargin, uint32 time, CBattleground * bg)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_WIN_BG );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqBGMap = ace->win_bg.bgMapID;
		uint32 ReqCount = ace->win_bg.winCount;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;
		// Wrong BG, continue on, kids.
		if( ReqBGMap != bgMapId )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		ad->completionTimeLast = time;
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				if( compareCriteria->raw.additionalRequirement1_type && scoreMargin < compareCriteria->raw.additionalRequirement1_type ) // BG Score Requirement.
					continue;

				// AV stuff :P
				if( bg->GetType() == BATTLEGROUND_ALTERAC_VALLEY )
				{
					AlteracValley * pAV = (AlteracValley*)bg;
					if( pAchievementEntry->ID == 225 ||  pAchievementEntry->ID == 1164) // AV: Everything Counts
					{
						continue; // We do not support mines yet in AV
					}

					if( pAchievementEntry->ID == 220 ) // AV: Stormpike Perfection
					{
						bool failure = false;
						// We must control all Alliance nodes and Horde nodes (towers only)
						for(uint32 i = 0; i < AV_NUM_CONTROL_POINTS; ++i)
						{
							if( pAV->GetNode(i)->IsGraveyard() )
								continue;

							if( pAV->GetNode(i)->GetState() != AV_NODE_STATE_ALLIANCE_CONTROLLED )
								failure = true;
						}
						if( failure ) continue;
					}

					if( pAchievementEntry->ID == 873 ) // AV: Frostwolf Perfection
					{
						bool failure = false;
						// We must control all Alliance nodes and Horde nodes (towers only)
						for(uint32 i = 0; i < AV_NUM_CONTROL_POINTS; ++i)
						{
							if( pAV->GetNode(i)->IsGraveyard() )
								continue;

							if( pAV->GetNode(i)->GetState() != AV_NODE_STATE_HORDE_CONTROLLED )
								failure = true;
						}
						if( failure ) continue;
					}
				}

				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaRequiresAchievement(uint32 achievementId)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_COMPLETE_ACHIEVEMENT );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqAchievement = ace->complete_achievement.linkedAchievement;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		if( ReqAchievement != achievementId )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaLevelUp(uint32 level)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_REACH_LEVEL );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqLevel = ace->reach_level.level;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		if( level < ReqLevel )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = m_player.getLevel();
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaOwnItem(uint32 itemId, uint32 stack)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_OWN_ITEM );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqItemId = ace->own_item.itemID;
		uint32 ReqItemCount = ace->own_item.itemCount;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		if( itemId != ReqItemId )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + stack;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaQuestCount(uint32 questCount)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_COMPLETE_QUEST_COUNT );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqQuestCount = ace->complete_quest_count.totalQuestCount;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = questCount;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaHonorableKillClass(uint32 classId)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_HK_CLASS );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqClass = ace->hk_class.classID;
		uint32 ReqCount = ace->hk_class.count;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		if( ReqClass != classId )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaHonorableKillRace(uint32 raceId)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_HK_RACE );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqRace = ace->hk_race.raceID;
		uint32 ReqCount = ace->hk_race.count;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		if( ReqRace != raceId )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaTalentResetCostTotal(uint32 cost)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_GOLD_SPENT_FOR_TALENTS );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + cost;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaTalentResetCount()
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_NUMBER_OF_TALENT_RESETS );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaBuyBankSlot(bool retroactive)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_BUY_BANK_SLOT );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqSlots = ace->buy_bank_slot.numberOfSlots; // We don't actually use this. :P

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				if( retroactive )
				{
					uint32 bytes = m_player.GetUInt32Value(PLAYER_BYTES_2);
					uint8 slots =(uint8) (bytes >> 16);
					ad->counter[i] = slots;
				}
				else
					ad->counter[i] = ad->counter[i] + 1;

				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaFlightPathsTaken()
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_FLIGHT_PATHS_TAKEN );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaHighestHealth(uint32 health)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_HEALTH );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace && ad->counter[i] < health)
			{
				ad->counter[i] = health;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaExploreArea(uint32 areaId, uint32 explorationFlags)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_EXPLORE_AREA );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqFlags = ace->explore_area.areaReference;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		WorldMapOverlayEntry * wmoe = dbcWorldMapOverlay.LookupEntryForced( ReqFlags );
		if(!wmoe) continue;

		AreaTable * at = dbcArea.LookupEntryForced(wmoe->AreaTableID);
		if(!at || !(ReqFlags & at->explorationFlag) )
			continue;

		uint32 offset = at->explorationFlag / 32;
		offset += PLAYER_EXPLORED_ZONES_1;

		uint32 val = (uint32)(1 << (at->explorationFlag % 32));
		uint32 currFields = m_player.GetUInt32Value(offset);

		// Not explored /sadface
		if( !(currFields & val) )
			continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaHonorableKill()
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_EARN_HONORABLE_KILL );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaDoEmote(uint32 emoteId, Unit * pTarget)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_DO_EMOTE );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqEmote = ace->do_emote.emoteID;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		// Target information is not stored, so we'll have to do this one by one...
		// --unless the target's name is the description of the criteria! Bahahaha
		if( pTarget && pTarget->IsCreature() )
		{
			Creature * pCreature = TO_CREATURE(pTarget);
			if( !ace->name || strlen(ace->name) == 0 || !pCreature->GetCreatureName() || stricmp(pCreature->GetCreatureName()->Name, ace->name) != 0 )
			{
				continue;
			}
			// Otherwise we go on happily and complete this achievement.
		}
		else
		{
			// Script individual ones here...
			if( ace->ID == 2379 ) // Make Love, Not Warcraft
			{
				if( !pTarget || !pTarget->IsPlayer() || !pTarget->isDead() )
					continue;
			}
			else
				continue;
		}

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaCompleteQuestsInZone(uint32 zoneId)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_COMPLETE_QUESTS_IN_ZONE );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqZone = ace->complete_quests_in_zone.zoneID;
		uint32 ReqCount = ace->complete_quests_in_zone.questCount;

		if( ReqZone != zoneId )
			continue;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = ad->counter[i] + 1;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}

void AchievementInterface::HandleAchievementCriteriaReachSkillLevel(uint32 skillId, uint32 skillLevel)
{
	AchievementCriteriaMap::iterator itr = objmgr.m_achievementCriteriaMap.find( ACHIEVEMENT_CRITERIA_TYPE_REACH_SKILL_LEVEL );
	AchievementCriteriaSet * acs = itr->second;
	if( !acs ) // We have no achievements for this criteria :(
		return;

	AchievementCriteriaSet::iterator citr = acs->begin();
	for(; citr != acs->end(); ++citr)
	{
		AchievementCriteriaEntry * ace = (*citr);
		uint32 AchievementID = ace->referredAchievement;
		uint32 ReqSkill = ace->reach_skill_level.skillID;
		uint32 ReqLevel = ace->reach_skill_level.skillLevel;

		if( ReqSkill != skillId )
			continue;

		AchievementEntry * pAchievementEntry = dbcAchievement.LookupEntryForced(AchievementID);
		if(!pAchievementEntry) continue;

		AchievementCriteriaEntry * compareCriteria = NULL;
		AchievementData * ad = GetAchievementDataByAchievementID(AchievementID);
		// Figure out our associative ID.
		for(uint32 i = 0; i < pAchievementEntry->AssociatedCriteriaCount; ++i)
		{
			compareCriteria = dbcAchivementCriteria.LookupEntry( pAchievementEntry->AssociatedCriteria[i] );			
			if( compareCriteria == ace )
			{
				ad->counter[i] = skillLevel;
				SendCriteriaUpdate(ad, i);
			}
		}

		if( CanCompleteAchievement(ad) )
			EventAchievementEarned(ad);
	}
}