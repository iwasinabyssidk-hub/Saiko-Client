/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>

namespace
{
constexpr int AVOID_FORCE_MS = 120;
int64_t s_LastAvoidTime = 0;
}

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aLastMousePos), std::end(m_aLastMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
	mem_zero(m_aAvoidForcing, sizeof(m_aAvoidForcing));
	mem_zero(m_aAvoidForcedDir, sizeof(m_aAvoidForcedDir));
	mem_zero(m_aAvoidForceUntil, sizeof(m_aAvoidForceUntil));
	mem_zero(m_aAvoidWasInDanger, sizeof(m_aAvoidWasInDanger));
}

void CControls::OnUpdate()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;
	if(!AutomationAllowed())
		return;

	const int Local = g_Config.m_ClDummy;

	m_aInputData[Local].m_Direction = 0;
	if(m_aInputDirectionLeft[Local] && !m_aInputDirectionRight[Local])
		m_aInputData[Local].m_Direction = -1;
	if(!m_aInputDirectionLeft[Local] && m_aInputDirectionRight[Local])
		m_aInputData[Local].m_Direction = 1;

	vec2 Pos = m_aMousePos[Local];
	if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
		if(MaxDistance > 5 && MaxDistance < 1000)
			Pos *= 1000.0f / (float)MaxDistance;
	}
	m_aInputData[Local].m_TargetX = (int)Pos.x;
	m_aInputData[Local].m_TargetY = (int)Pos.y;
	if(!m_aInputData[Local].m_TargetX && !m_aInputData[Local].m_TargetY)
		m_aInputData[Local].m_TargetX = 1;

	if(g_Config.m_Miki)
		AvoidFreeze();
	if(g_Config.m_MikiPrime)
		HookAssist();
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
	mem_zero(m_aAvoidForcing, sizeof(m_aAvoidForcing));
	mem_zero(m_aAvoidForcedDir, sizeof(m_aAvoidForcedDir));
	mem_zero(m_aAvoidForceUntil, sizeof(m_aAvoidForceUntil));
	mem_zero(m_aAvoidWasInDanger, sizeof(m_aAvoidWasInDanger));
	s_LastAvoidTime = 0;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if(g_Config.m_TcHideChatBubbles && Client()->RconAuthed())
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		if(m_aAvoidForcing[g_Config.m_ClDummy])
		{
			const int Local = g_Config.m_ClDummy;
			const int64_t Now = time_get();
			if(Now <= m_aAvoidForceUntil[Local])
			{
				int IntendedDir = 0;
				if(m_aInputDirectionLeft[Local] && !m_aInputDirectionRight[Local])
					IntendedDir = -1;
				else if(!m_aInputDirectionLeft[Local] && m_aInputDirectionRight[Local])
					IntendedDir = 1;

				bool Release = false;
				if(!g_Config.m_Miki)
				{
					Release = true;
				}
				else if(IntendedDir != m_aAvoidForcedDir[Local])
				{
					CNetObj_PlayerInput Test = m_aInputData[Local];
					Test.m_Direction = IntendedDir;
					if(!PredictFreeze(Test, AvoidPredictTicks()))
						Release = true;
				}

				if(!Release)
					m_aInputData[Local].m_Direction = m_aAvoidForcedDir[Local];
				else
					m_aAvoidForcing[Local] = false;
			}
			else
			{
				m_aAvoidForcing[Local] = false;
			}
		}

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == g_Config.m_ClDummy)
		{
			if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
			TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	if(NewInput[0] || NewInput[1])
		return true;
	else
		return false;
}

bool CControls::AutomationAllowed() const
{
	if(!GameClient() || !Collision() || !GameClient()->Predict())
		return false;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive())
		return false;
	return true;
}

int CControls::AvoidPredictTicks() const
{
	return std::clamp(g_Config.m_Miki2, 1, 100);
}

int CControls::HookAssistTicks() const
{
	return std::clamp(g_Config.m_Miki6, 1, 100);
}

int CControls::DirectionSensitivityStep() const
{
	return g_Config.m_Miki3 >= 67 ? 2 : 1;
}

float CControls::DirectionSensitivityFactor() const
{
	return std::clamp(g_Config.m_Miki3 / 100.0f, 0.10f, 1.0f);
}

int CControls::MaxAvoidAttempts() const
{
	return std::clamp(g_Config.m_Miki4, 1, 100);
}

int CControls::MaxAvoidAttemptsPerDirection() const
{
	return std::clamp(g_Config.m_Miki5, 1, 100);
}

bool CControls::GetFreeze(vec2 Pos, int FreezeTime) const
{
	const int MapIndex = Collision()->GetPureMapIndex(Pos.x, Pos.y);
	return FreezeTime > 0 || Collision()->IsTeleport(MapIndex) || Collision()->IsCheckEvilTeleport(MapIndex) ||
		Collision()->IsCheckTeleport(MapIndex) || Collision()->IsEvilTeleport(MapIndex);
}

bool CControls::IsAvoidCooldownElapsed(int64_t CurrentTime) const
{
	const int64_t ConfiguredDelay = static_cast<int64_t>(std::max(0, g_Config.m_Miki1)) * time_freq() / 1000;
	if(s_LastAvoidTime == 0)
		return true;
	return CurrentTime - s_LastAvoidTime >= ConfiguredDelay;
}

void CControls::UpdateAvoidCooldown(int64_t CurrentTime)
{
	s_LastAvoidTime = CurrentTime + static_cast<int64_t>(AVOID_FORCE_MS) * time_freq() / 1000;
}

bool CControls::PredictFreeze(const CNetObj_PlayerInput &Input, int Ticks, int *pDangerTick, float *pDangerDistance) const
{
	if(!GameClient()->Predict())
	{
		if(pDangerTick)
			*pDangerTick = -1;
		if(pDangerDistance)
			*pDangerDistance = 0.0f;
		return false;
	}

	static CGameWorld s_World;
	s_World.CopyWorldClean(&GameClient()->m_PredictedWorld);

	const int Local = g_Config.m_ClDummy;
	const int ClientId = GameClient()->m_aLocalIds[Local];
	CCharacter *pChar = s_World.GetCharacterById(ClientId);
	if(!pChar)
	{
		if(pDangerTick)
			*pDangerTick = -1;
		if(pDangerDistance)
			*pDangerDistance = 0.0f;
		return false;
	}

	const vec2 StartPos = pChar->m_Pos;

	pChar->OnDirectInput(&Input);
	const int Steps = std::max(1, Ticks);
	for(int i = 0; i < Steps; i++)
	{
		pChar->OnPredictedInput(&Input);
		s_World.m_GameTick++;
		s_World.Tick();
		if(GetFreeze(pChar->m_Pos, pChar->m_FreezeTime))
		{
			if(pDangerTick)
				*pDangerTick = i + 1;
			if(pDangerDistance)
				*pDangerDistance = distance(StartPos, pChar->m_Pos);
			return true;
		}
	}

	if(pDangerTick)
		*pDangerTick = -1;
	if(pDangerDistance)
		*pDangerDistance = 0.0f;
	return false;
}

bool CControls::TryMove(const CNetObj_PlayerInput &BaseInput, int Direction, int CheckTicks)
{
	CNetObj_PlayerInput ModifiedInput = BaseInput;
	if(BaseInput.m_Direction != Direction)
	{
		const int Sensitivity = DirectionSensitivityStep();
		if(Direction > BaseInput.m_Direction)
			ModifiedInput.m_Direction = std::min(BaseInput.m_Direction + Sensitivity, Direction);
		else
			ModifiedInput.m_Direction = std::max(BaseInput.m_Direction - Sensitivity, Direction);
	}

	if(!PredictFreeze(ModifiedInput, CheckTicks))
	{
		const int Local = g_Config.m_ClDummy;
		m_aAvoidForcing[Local] = true;
		m_aAvoidForcedDir[Local] = ModifiedInput.m_Direction;
		m_aAvoidForceUntil[Local] = time_get() + static_cast<int64_t>(AVOID_FORCE_MS) * time_freq() / 1000;
		return true;
	}
	return false;
}

bool CControls::TryAvoidFreeze(int LocalPlayerId)
{
	const int CheckTicks = AvoidPredictTicks();
	const int MaxAttempts = MaxAvoidAttempts();
	const int MaxAttemptsPerDirection = MaxAvoidAttemptsPerDirection();
	const CNetObj_PlayerInput BaseInput = m_aInputData[LocalPlayerId];

	const CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_aLocalIds[LocalPlayerId]);
	vec2 DangerDir(0.0f, 0.0f);
	if(pChar && pChar->Core())
	{
		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PredictFreeze(BaseInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			static CGameWorld s_World;
			s_World.CopyWorldClean(&GameClient()->m_PredictedWorld);
			const int ClientId = GameClient()->m_aLocalIds[LocalPlayerId];
			CCharacter *pTestChar = s_World.GetCharacterById(ClientId);
			if(pTestChar)
			{
				pTestChar->OnDirectInput(&BaseInput);
				for(int i = 0; i < std::min(DangerTick, CheckTicks); i++)
				{
					pTestChar->OnPredictedInput(&BaseInput);
					s_World.m_GameTick++;
					s_World.Tick();
				}
				const vec2 DangerPos = pTestChar->m_Pos;
				const vec2 CurrentPos = pChar->Core()->m_Pos;
				const vec2 DirVec = DangerPos - CurrentPos;
				const float DirLen = length(DirVec);
				if(DirLen > 0.1f)
					DangerDir = DirVec / DirLen;
			}
		}
	}

	std::array<int, 3> aDirections = {-1, 1, 0};
	if(std::fabs(DangerDir.x) > 0.1f)
	{
		const int AwayDir = DangerDir.x > 0.0f ? -1 : 1;
		const int TowardDir = DangerDir.x > 0.0f ? 1 : -1;
		aDirections = {AwayDir, TowardDir, 0};
	}

	std::array<int, 3> aDirectionAttempts = {0, 0, 0};
	int Attempts = 0;

	while(Attempts < MaxAttempts)
	{
		bool TriedAny = false;
		for(size_t i = 0; i < aDirections.size() && Attempts < MaxAttempts; i++)
		{
			if(aDirectionAttempts[i] >= MaxAttemptsPerDirection)
				continue;

			const int Direction = aDirections[i];
			if(Direction == BaseInput.m_Direction && aDirectionAttempts[i] == 0)
			{
				aDirectionAttempts[i]++;
				continue;
			}

			CNetObj_PlayerInput AttemptInput = BaseInput;
			if(aDirectionAttempts[i] > 0 && AttemptInput.m_Hook != 0)
				AttemptInput.m_Hook = 0;

			const int AttemptCheckTicks = std::max(1, CheckTicks - aDirectionAttempts[i]);
			aDirectionAttempts[i]++;
			Attempts++;
			TriedAny = true;

			if(TryMove(AttemptInput, Direction, AttemptCheckTicks))
			{
				CNetObj_PlayerInput VerifyInput = AttemptInput;
				if(VerifyInput.m_Direction != Direction)
				{
					const int Sensitivity = DirectionSensitivityStep();
					if(Direction > VerifyInput.m_Direction)
						VerifyInput.m_Direction = std::min(VerifyInput.m_Direction + Sensitivity, Direction);
					else
						VerifyInput.m_Direction = std::max(VerifyInput.m_Direction - Sensitivity, Direction);
				}

				if(!PredictFreeze(VerifyInput, std::min(AttemptCheckTicks + 2, 15)))
					return true;

				m_aAvoidForcing[LocalPlayerId] = false;
			}
		}

		if(!TriedAny)
			break;
	}

	return false;
}

bool CControls::IsMouseMoved(int LocalPlayerId) const
{
	const bool HasMoved = m_aMousePos[LocalPlayerId] != m_aLastMousePos[LocalPlayerId];
	m_aLastMousePos[LocalPlayerId] = m_aMousePos[LocalPlayerId];
	return HasMoved;
}

bool CControls::IsPlayerActive(int LocalPlayerId) const
{
	const CNetObj_PlayerInput &Input = m_aInputData[LocalPlayerId];
	return Input.m_Direction != 0 || Input.m_Jump != 0 || Input.m_Hook != 0 || IsMouseMoved(LocalPlayerId);
}

void CControls::AvoidFreeze()
{
	if(!g_Config.m_Miki)
		return;

	const int64_t CurrentTime = time_get();
	if(!IsAvoidCooldownElapsed(CurrentTime))
		return;

	const int LocalPlayerId = g_Config.m_ClDummy;

	if(m_aAvoidForcing[LocalPlayerId] && CurrentTime <= m_aAvoidForceUntil[LocalPlayerId])
		return;
	if(!IsPlayerActive(LocalPlayerId))
		return;

	const CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_aLocalIds[LocalPlayerId]);
	if(pChar && pChar->Core())
	{
		const vec2 Pos = pChar->Core()->m_Pos;
		const float VelY = pChar->Core()->m_Vel.y;

		if(VelY > 2.0f)
		{
			const float GroundCheckDist = 64.0f;
			bool HasEscapePath = false;

			for(int Dir = -1; Dir <= 1; Dir += 2)
			{
				const float CheckX = Pos.x + Dir * 32.0f;
				const float CheckY = Pos.y + GroundCheckDist;

				bool PathClear = true;
				for(float Y = Pos.y; Y <= CheckY; Y += 16.0f)
				{
					const int MapIndex = Collision()->GetPureMapIndex(CheckX, Y);
					const int TileIndex = Collision()->GetTileIndex(MapIndex);
					const int TileFIndex = Collision()->GetFrontTileIndex(MapIndex);
					if(TileIndex == TILE_FREEZE || TileFIndex == TILE_FREEZE ||
						TileIndex == TILE_DFREEZE || TileFIndex == TILE_DFREEZE ||
						TileIndex == TILE_LFREEZE || TileFIndex == TILE_LFREEZE)
					{
						PathClear = false;
						break;
					}
				}

				if(PathClear)
				{
					HasEscapePath = true;
					break;
				}
			}

			if(!HasEscapePath)
			{
				m_aAvoidWasInDanger[LocalPlayerId] = false;
				return;
			}
		}
	}

	int DangerTick = -1;
	float DangerDistance = 0.0f;
	const bool InDanger = PredictFreeze(m_aInputData[LocalPlayerId], AvoidPredictTicks(), &DangerTick, &DangerDistance);
	if(!InDanger)
	{
		m_aAvoidWasInDanger[LocalPlayerId] = false;
		return;
	}

	const int Level = std::clamp(AvoidPredictTicks(), 1, 5);
	const float SensitivityFactor = DirectionSensitivityFactor();

	const float BaseTickLimit = Level <= 2 ? (AvoidPredictTicks() / 4.0f) :
		(Level <= 4 ? (AvoidPredictTicks() / 3.0f) : (AvoidPredictTicks() / 2.0f));
	const float SensitivityAdjustment = 0.5f + (0.5f * SensitivityFactor);
	const int CloseTickLimit = std::max(1, (int)(BaseTickLimit * SensitivityAdjustment));

	float BaseDistanceThreshold = 48.0f;
	if(Level == 2)
		BaseDistanceThreshold = 64.0f;
	else if(Level == 3)
		BaseDistanceThreshold = 80.0f;
	else if(Level == 4)
		BaseDistanceThreshold = 96.0f;
	else if(Level >= 5)
		BaseDistanceThreshold = 112.0f;

	const float DistanceThreshold = BaseDistanceThreshold * (0.6f + (0.8f * SensitivityFactor));
	if(DangerTick > CloseTickLimit && DangerDistance > DistanceThreshold)
	{
		m_aAvoidWasInDanger[LocalPlayerId] = false;
		return;
	}

	if(m_aAvoidWasInDanger[LocalPlayerId])
		return;

	if(TryAvoidFreeze(LocalPlayerId))
	{
		m_aAvoidWasInDanger[LocalPlayerId] = true;
		UpdateAvoidCooldown(CurrentTime);
	}
}

void CControls::HookAssist()
{
	if(!g_Config.m_MikiPrime)
		return;

	static int s_aLastHookTick[NUM_DUMMIES] = {-1, -1};
	const int Local = g_Config.m_ClDummy;
	const int CurrentPredTick = GameClient()->m_PredictedWorld.GameTick();
	if(CurrentPredTick == s_aLastHookTick[Local])
		return;
	s_aLastHookTick[Local] = CurrentPredTick;

	const int ClientId = GameClient()->m_aLocalIds[Local];
	const CCharacter *pChar = ClientId >= 0 ? GameClient()->m_PredictedWorld.GetCharacterById(ClientId) : nullptr;
	const CCharacterCore *pCore = pChar ? pChar->Core() : nullptr;
	if(!pChar || !pCore)
		return;

	const bool AimingUp = m_aInputData[Local].m_TargetY < 0;
	const int BaseCheckTicks = HookAssistTicks();
	const int CheckTicks = std::max(1, BaseCheckTicks + ((BaseCheckTicks >= 1 && BaseCheckTicks <= 3 && AimingUp) ? 1 : 0));
	const float SensitivityFactor = std::clamp(BaseCheckTicks / 10.0f, 0.2f, 1.0f);
	const bool CurrentlyHoldingHook = m_aInputData[Local].m_Hook != 0;
	const vec2 Vel = pCore->m_Vel;
	const float Speed = length(Vel);

	if(CurrentlyHoldingHook)
	{
		const bool IsJumping = m_aInputData[Local].m_Jump != 0;
		const vec2 Pos = pCore->m_Pos;
		const float CheckOffset = 16.0f;
		const bool WallLeft = Collision()->CheckPoint(Pos.x - CheckOffset, Pos.y);
		const bool WallRight = Collision()->CheckPoint(Pos.x + CheckOffset, Pos.y);
		const bool InOneTileSpace = WallLeft && WallRight;

		const bool AimingLeftUp = m_aInputData[Local].m_TargetX < 0 && m_aInputData[Local].m_TargetY < 0;
		const bool AimingRightUp = m_aInputData[Local].m_TargetX > 0 && m_aInputData[Local].m_TargetY < 0;
		const bool MovingRight = m_aInputData[Local].m_Direction > 0;
		const bool MovingLeft = m_aInputData[Local].m_Direction < 0;
		const bool DiagonalHookInOneTile = InOneTileSpace && ((AimingLeftUp && MovingRight) || (AimingRightUp && MovingLeft));
		if(IsJumping || DiagonalHookInOneTile)
			return;

		CNetObj_PlayerInput TestInput = m_aInputData[Local];
		TestInput.m_Hook = 1;

		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PredictFreeze(TestInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			CNetObj_PlayerInput ReleaseInput = m_aInputData[Local];
			ReleaseInput.m_Hook = 0;
			const bool CanAvoidByReleasing = !PredictFreeze(ReleaseInput, CheckTicks);
			if(!CanAvoidByReleasing && DangerTick > CheckTicks / 2)
				return;

			const float SpeedFactor = std::min(1.0f, Speed / 10.0f);
			const float UrgencyMultiplier = 0.3f + (0.7f * SensitivityFactor) - (0.2f * SpeedFactor);
			const float DistanceMultiplier = 0.5f + (1.5f * SensitivityFactor) + (0.5f * SpeedFactor);
			const int HookUrgencyTicks = std::max(1, (int)(CheckTicks * UrgencyMultiplier));
			const float HookDistanceThreshold = 32.0f * (1.5f + BaseCheckTicks * DistanceMultiplier);
			if((DangerTick <= HookUrgencyTicks || DangerDistance <= HookDistanceThreshold) && CanAvoidByReleasing)
				m_aInputData[Local].m_Hook = 0;
		}
	}
	else
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Local];
		TestInput.m_Hook = 1;

		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PredictFreeze(TestInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			CNetObj_PlayerInput NoHookInput = m_aInputData[Local];
			NoHookInput.m_Hook = 0;
			const bool CanMoveWithoutHook = !PredictFreeze(NoHookInput, CheckTicks);
			const float UrgencyMultiplier = 0.2f + (0.6f * SensitivityFactor);
			const int HookUrgencyTicks = std::max(1, (int)(CheckTicks * UrgencyMultiplier));
			if(DangerTick <= HookUrgencyTicks && DangerDistance < 48.0f && CanMoveWithoutHook)
				m_aInputData[Local].m_Hook = 0;
		}
	}
}
