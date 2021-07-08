/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * =============================================================================
 * Pounce Damage Uncap
 * Copyright (C) 2011 Michael "ProdigySim" Busby
 * All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the authors give you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 */

#include <stdio.h>
#include "pounce_damage_uncap.h"
#include "memutils.h"
#include "sourcehook.h"
#include "icvar.h"
#include "tier1/iconvar.h"
#include "tier1/convar.h"

PounceDamageUncap g_PounceDamageUncap;
IServerGameDLL *server = NULL;
ICvar* g_pCVar = NULL;

bool PatchPounceVars(void * pServerDll);
void recalculate_difference();
void min_range_changed(IConVar *var, const char *pOldValue, float flOldValue);
void max_range_changed(IConVar *var, const char *pOldValue, float flOldValue);
ConVar z_pounce_damage_range_max("z_pounce_damage_range_max", "1000.0", FCVAR_GAMEDLL|FCVAR_CHEAT, "Range at which a pounce is worth the maximum bonus damage.", true, 0.0, false, 0.0, max_range_changed);
ConVar z_pounce_damage_range_min("z_pounce_damage_range_min", "300.0", FCVAR_GAMEDLL|FCVAR_CHEAT, "Minimum range for a pounce to be worth bonus damage.", true, 0.0, false, 0.0, min_range_changed);


// We're looking at CTerrorPlayer::OnPouncedOnSurvivor, right before a log print that says "Pounce by %s dealt %0.1f damage from a 2d distance of %.0f\n"
// There are a bunch of comparisons against "300.0f", "1000.0f" and some math using the ratio of 1/700--used to bounds check the pounce distance, then convert it to a percent
// and scale the damage (z_hunter_max_pounce_bonus_damage).

// The idea of these patches is to patch out the loading of these values from memory to read from our own variables instead. Luckily,
// all of the instructions are reading from the data section already--so we just have to patch the address in the instructions.
// Future version of l4d2 may inline those numbers instead of reading from memory--which would require a different approach.


#if SH_SYS == SH_SYS_WIN32

// This segment we search for is the start of the first instruction that loads 300.0f into memory. 
// It's just before the log line about pounce damage dealt essentially.
//movss   xmm3, ds:fl_300 (masked addr)
//mulss   xmm0, xmm0
//mulss   xmm1, xmm1
// F3 0F 10 1D ? ? ? ? F3 0F 59 C0 F3 0F 59 C9
const char c_sPattern[] = "\xF3\x0F\x10\x1D\x2A\x2A\x2A\x2A\xF3\x0F\x59\xC0\xF3\x0F\x59\xC9";


//movss   xmm3, ds:fl_300
// instruction starts at sig start, addr is 4 bytes into instruction.
#define  _MinRangeAddrOffset 4

//comiss  xmm0, ds:fl_1000
// 76 bytes from sig is this instruction.
// address operand is 3 bytes into that instruction = 79 bytes.
#define _MaxRangeAddrOffset 79

//mulss   xmm0, ds:fl_1_div_700
// 119 bytes from the sig
// address operand is 4 bytes into that instruction = 123 bytes.
#define _RangeScaleFactorAddrOffset 123

#elif SH_SYS == SH_SYS_LINUX
// This segment we search for is the start of the first instruction that loads 300.0f into memory. 
// It's just before the log line about pounce damage dealt essentially.
// comiss  xmm0, ds:flt_BAA9BC (masked)
// jbe     loc_95EF20
// movss   xmm1, [ebp+var_19C] (only the instruction)
// 0F 2F 05 ? ? ? ? 0F 86 8A 02 00 00 A1 BC
const char c_sPattern[] = "\x0F\x2F\x05\x2A\x2A\x2A\x2A\x0F\x86\x8A\x02\x00\x00\xA1\xBC";

//comiss  xmm0, ds:fl_300
// instruction starts at sig start, addr is 3 bytes into instruction
#define _MinRangeAddrOffset 3

//comiss  xmm1, ds:fl_1000
// 18 bytes from sig is this instruction
// address operand is 3 bytes into that instruction = 21 bytes
#define _MaxRangeAddrOffset 21

//mulss   xmm0, ds:fl_1_div_700
// 1767 bytes from sig is this instruction
// address operand is 4 bytes into that instruction = 1771 bytes
#define _RangeScaleFactorAddrOffset 1771

//movss   xmm1, ds:fl_neg_300
// 1778 bytes from sig is this instruction
// address operand is 4 bytes into that instruction = 1782 bytes
#define _NegativeMinRangeAddrOffset 1782

#endif

char *pPatchBaseAddr=NULL;

// Places to store the original addresses for unpatching
float * g_pMinRangeData=NULL;
float * g_pMaxRangeData=NULL;
float * g_pRangeScaleFactorData=NULL;


// Places to store our versions of the numbers
float g_flMinRange=300.0;
float g_flMaxRange=1000.0;
// Range scale factor is (1.0 / (MaxRange - MinRange))--1 distance unit equals this portion of max_pounce_bonus_damage.
float g_fRangeScaleFactor=1.0/700.0;

#ifdef _NegativeMinRangeAddrOffset
// On linux, the value "-300.0" is used instead of just subtracting the "300.0" that has been already loaded.
// So we need to store/patch an additional value in memory

// old addr for unpatching
float * g_pNegativeMinRangeData=NULL;
// -300.0
float g_NegativeMinRange = -300.0;
#endif


PLUGIN_EXPOSE(PDUncap, g_PounceDamageUncap);

bool PounceDamageUncap::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);

	if(!PatchPounceVars(server))
	{
		Warning("Couldn't patch pounce variables. Giving up.\n");
		return false;
	}

	ConVar_Register(0, this);

	g_flMinRange = z_pounce_damage_range_min.GetFloat();
	g_flMaxRange = z_pounce_damage_range_max.GetFloat();
	recalculate_difference();

	return true;
}

bool PatchPounceVars(void * pServerDll)
{
	// Find the address of the start of our signature
	char *pAddr = pPatchBaseAddr = (char*)g_MemUtils.FindLibPattern(pServerDll, c_sPattern, sizeof(c_sPattern)-1);
	if(pAddr == NULL)
	{
		return false;
	}
	DevMsg("Found Pattern at %08x\n", pAddr);

	// Patch area length = last offset + (addr_length)
#ifdef _NegativeMinRangeAddrOffset
	int patchAreaLength = _NegativeMinRangeAddrOffset + sizeof(float*);
#else
	int patchAreaLength = _RangeScaleFactorAddrOffset + sizeof(float*);
#endif

	if(!g_MemUtils.SetMemPatchable(pAddr, patchAreaLength))
	{
		Warning("Failed to set mem patchable\n");
		return false;
	}

	// pPatchAddr points to the start of our 4 byte address we will patch.

	// Patch min range address read
	float ** pPatchAddr = (float**)(pAddr + _MinRangeAddrOffset);
	g_pMinRangeData = *pPatchAddr;
	g_flMinRange=*g_pMinRangeData;
	*pPatchAddr=&g_flMinRange;

	// Patch max range address read
	pPatchAddr = (float**)(pAddr + _MaxRangeAddrOffset);
	g_pMaxRangeData = *pPatchAddr;
	g_flMaxRange=*g_pMaxRangeData;
	*pPatchAddr=&g_flMaxRange;

	pPatchAddr = (float**)(pAddr + _RangeScaleFactorAddrOffset);
	g_pRangeScaleFactorData = *pPatchAddr;
	g_fRangeScaleFactor=*g_pRangeScaleFactorData;
	*pPatchAddr=&g_fRangeScaleFactor;

#ifdef _NegativeMinRangeAddrOffset
	// This patching should be linux only. Where -300.0 is precalculated, we have to replace that value.
	pPatchAddr = (float**)(pAddr + _NegativeMinRangeAddrOffset);
	g_pNegativeMinRangeData = *pPatchAddr;
	g_NegativeMinRange=*g_pNegativeMinRangeData;
	*pPatchAddr=&g_NegativeMinRange;
#endif

	return true;
}

void UnPatchPounceVars()
{
	char *pAddr = pPatchBaseAddr;
	if(pAddr == NULL)
	{
		return;
	}
	// Revert the address reads to their original address values

	// Unpatch minrange
	float ** pPatchAddr = (float**)(pAddr + _MinRangeAddrOffset);
	*pPatchAddr=g_pMinRangeData;

	pPatchAddr = (float**)(pAddr + _MaxRangeAddrOffset);
	*pPatchAddr=g_pMaxRangeData;

	pPatchAddr = (float**)(pAddr + _RangeScaleFactorAddrOffset);
	*pPatchAddr=g_pRangeScaleFactorData;

#ifdef _NegativeMinRangeAddrOffset
	pPatchAddr = (float**)(pAddr + _NegativeMinRangeAddrOffset);
	*pPatchAddr=g_pNegativeMinRangeData;
#endif
}

bool PounceDamageUncap::Unload(char *error, size_t maxlen)
{
	UnPatchPounceVars();
	return true;
}

void recalculate_difference()
{
	float diff = g_flMaxRange - g_flMinRange;
	g_fRangeScaleFactor = (diff == 0.0) ? FLT_MAX : 1.0/diff;
#ifdef _NegativeMinRangeAddrOffset
	g_NegativeMinRange = -g_flMinRange;
#endif
}

void min_range_changed(IConVar *var, const char *pOldValue, float flOldValue)
{
	g_flMinRange = z_pounce_damage_range_min.GetFloat();
	recalculate_difference();
}

void max_range_changed(IConVar *var, const char *pOldValue, float flOldValue)
{
	g_flMaxRange = z_pounce_damage_range_max.GetFloat();
	recalculate_difference();
}

const char *PounceDamageUncap::GetLicense()
{
	return "GPLv3";
}

const char *PounceDamageUncap::GetVersion()
{
	return "1.1.0.0";
}

const char *PounceDamageUncap::GetDate()
{
	return __DATE__;
}

const char *PounceDamageUncap::GetLogTag()
{
	return "PDUNCAP";
}

const char *PounceDamageUncap::GetAuthor()
{
	return "Michael \"ProdigySim\" Busby";
}

const char *PounceDamageUncap::GetDescription()
{
	return "Patch L4D2 to allow uncapping the pounce range limits.";
}

const char *PounceDamageUncap::GetName()
{
	return "Pounce Damage Uncap";
}

const char *PounceDamageUncap::GetURL()
{
	return "https://github.com/ProdigySim/Pounce-Damage-Uncap";
}

bool PounceDamageUncap::RegisterConCommandBase(ConCommandBase *pVar)
{
	return META_REGCVAR(pVar);
}
