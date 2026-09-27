////////////////////////////////////////////////////////////////////////////
//	Module 		: script_sound_inline.h
//	Created 	: 06.02.2004
//  Modified 	: 06.02.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay Script sound class inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC u32 CScriptSound::Length()
{
#ifdef DEDICATED_SERVER
	return 0;
#else
	VERIFY(m_sound._handle());
	return iFloor(m_sound.get_length_sec() * 1000.0f);
#endif // DEDICATED_SERVER
}

IC void CScriptSound::Play(CScriptGameObject* object)
{
	Play(object, 0.f, 0);
}

IC void CScriptSound::Play(CScriptGameObject* object, float delay)
{
	Play(object, delay, 0);
}

IC void CScriptSound::PlayAtPos(CScriptGameObject* object, const Fvector& position)
{
	PlayAtPos(object, position, 0.f, 0);
}

IC void CScriptSound::PlayAtPos(CScriptGameObject* object, const Fvector& position, float delay)
{
	PlayAtPos(object, position, delay, 0);
}

IC void CScriptSound::SetMinDistance(const float fMinDistance)
{
#ifdef DEDICATED_SERVER
	(void)fMinDistance;
#else
	VERIFY(m_sound._handle());
	m_sound.set_range(fMinDistance, GetMaxDistance());
#endif // DEDICATED_SERVER
}

IC void CScriptSound::SetMaxDistance(const float fMaxDistance)
{
#ifdef DEDICATED_SERVER
	(void)fMaxDistance;
#else
	VERIFY(m_sound._handle());
	m_sound.set_range(GetMinDistance(), fMaxDistance);
#endif // DEDICATED_SERVER
}

IC const float CScriptSound::GetFrequency() const
{
#ifdef DEDICATED_SERVER
	return 1.0f;
#else
	VERIFY(m_sound._handle());
	return (m_sound.get_params()->freq);
#endif // DEDICATED_SERVER
}

IC const float CScriptSound::GetMinDistance() const
{
#ifdef DEDICATED_SERVER
	return 0.0f;
#else
	VERIFY(m_sound._handle());
	return (m_sound.get_params()->min_distance);
#endif // DEDICATED_SERVER
}

IC const float CScriptSound::GetMaxDistance() const
{
#ifdef DEDICATED_SERVER
	return 0.0f;
#else
	VERIFY(m_sound._handle());
	return (m_sound.get_params()->max_distance);
#endif // DEDICATED_SERVER
}

IC const float CScriptSound::GetVolume() const
{
#ifdef DEDICATED_SERVER
	return 1.0f;
#else
	VERIFY(m_sound._handle());
	return (m_sound.get_params()->volume);
#endif // DEDICATED_SERVER
}

IC bool CScriptSound::IsPlaying() const
{
	#ifdef DEDICATED_SERVER
	return false;
	#else
	//  commented for comfort work with -nosound command line option
	//	VERIFY				(m_sound._handle());
	return (!!m_sound._feedback());
	#endif // DEDICATED_SERVER
}

IC void CScriptSound::AttachTail(LPCSTR caSoundName)
{
#ifdef DEDICATED_SERVER
	(void)caSoundName;
#else
	m_sound.attach_tail(caSoundName);
#endif // DEDICATED_SERVER
}

IC void CScriptSound::Stop()
{
#ifndef DEDICATED_SERVER
	VERIFY(m_sound._handle());
	m_sound.stop();
#endif // DEDICATED_SERVER
}

IC void CScriptSound::StopDeffered()
{
#ifndef DEDICATED_SERVER
	VERIFY(m_sound._handle());
	m_sound.stop_deffered();
#endif // DEDICATED_SERVER
}

IC void CScriptSound::SetPosition(const Fvector& position)
{
#ifdef DEDICATED_SERVER
	(void)position;
#else
	VERIFY(m_sound._handle());
	m_sound.set_position(position);
#endif // DEDICATED_SERVER
}

IC void CScriptSound::SetFrequency(float frequency)
{
#ifdef DEDICATED_SERVER
	(void)frequency;
#else
	VERIFY(m_sound._handle());
	m_sound.set_frequency(frequency);
#endif // DEDICATED_SERVER
}

IC void CScriptSound::SetVolume(float volume)
{
#ifdef DEDICATED_SERVER
	(void)volume;
#else
	VERIFY(m_sound._handle());
	m_sound.set_volume(volume);
#endif // DEDICATED_SERVER
}

IC const CSound_params* CScriptSound::GetParams()
{
#ifdef DEDICATED_SERVER
	return nullptr;
#else
	VERIFY(m_sound._handle());
	return (m_sound.get_params());
#endif // DEDICATED_SERVER
}

IC void CScriptSound::SetParams(CSound_params* sound_params)
{
#ifdef DEDICATED_SERVER
	(void)sound_params;
#else
	VERIFY(m_sound._handle());
	m_sound.set_params(sound_params);
#endif // DEDICATED_SERVER
}
