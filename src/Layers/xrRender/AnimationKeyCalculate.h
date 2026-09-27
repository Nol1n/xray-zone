#pragma once
//------------------------------------------------------------------------------
// calculate
//------------------------------------------------------------------------------
IC void KEY_Interp(CKey& D, const CKey& K1, const CKey& K2, float delta)
{
	VERIFY(_valid(delta));
	VERIFY(delta>=0.f && delta<=1.f);
	D.Q.slerp(K1.Q, K2.Q, delta);
	D.T.lerp(K1.T, K2.T, delta);
}



/*
ICF float smooth(float x)
{
    float x0	= x*2.f-1.f;
    float s 	= (x0<0.f)?-1.f:1.f;

    return ((s*pow(_abs(x0),1.f/1.5f))+1.f)/2.f;
}
*/
IC void QR2Quat(const CKeyQR& K, Fquaternion& Q)
{
	Q.x = float(K.x) * KEY_QuantI;
	Q.y = float(K.y) * KEY_QuantI;
	Q.z = float(K.z) * KEY_QuantI;
	Q.w = float(K.w) * KEY_QuantI;
}

IC void QT8_2T(const CKeyQT8& K, const CMotion& M, Fvector& T)
{
	T.x = float(K.x1) * M._sizeT.x + M._initT.x;
	T.y = float(K.y1) * M._sizeT.y + M._initT.y;
	T.z = float(K.z1) * M._sizeT.z + M._initT.z;
}

IC void QT16_2T(const CKeyQT16& K, const CMotion& M, Fvector& T)
{
	T.x = float(K.x1) * M._sizeT.x + M._initT.x;
	T.y = float(K.y1) * M._sizeT.y + M._initT.y;
	T.z = float(K.z1) * M._sizeT.z + M._initT.z;
}

IC void Dequantize(CKey& K, const CBlend& BD, const CMotion& M)
{
	EvaluateMotionKey(K, BD.timeCurrent, M);
}


IC void MixInterlerp(CKey& Result, const CKey* R, const CBlend* const BA[MAX_BLENDED], int b_count)
{
	VERIFY(MAX_BLENDED >= b_count);
	float weights[MAX_BLENDED];
	for (int i = 0; i < b_count; ++i)
		weights[i] = BA[i]->blendAmount;
	BlendMotionKeys(Result, R, weights, b_count);
}

IC void key_sub(CKey& rk, const CKey& k0, const CKey& k1) //sub right
{
	Fquaternion q;
	q.inverse(k1.Q);
	rk.Q.mul(k0.Q, q);
	//rk.Q.normalize();//?
	rk.T.sub(k0.T, k1.T);
}

IC void key_identity(CKey& k)
{
	k.Q.identity();
	k.T.set(0, 0, 0);
}

IC void key_add(CKey& res, const CKey& k0, const CKey& k1) //add right
{
	res.Q.set(Fquaternion().mul(k0.Q, k1.Q));
	//res.Q.normalize();
	res.T.add(k0.T, k1.T);
}

IC void q_scale(Fquaternion& q, float v)
{
	float angl;
	Fvector ax;
	q.get_axis_angle(ax, angl);
	q.rotation(ax, angl * v);
	//q.normalize();
}

IC void key_scale(CKey& res, const CKey& k, float v)
{
	res = k;
	q_scale(res.Q, v);
	res.T.mul(v);
}

IC void key_mad(CKey& res, const CKey& k0, const CKey& k1, float v)
{
	CKey k;
	key_scale(k, k1, v);
	key_add(res, k, k0);
}


IC void keys_substruct(CKey* R, const CKey* BR, int b_count)
{
	for (int i = 0; i < b_count; i++)
	{
		CKey r;
		key_sub(r, R[i], BR[i]);
		R[i] = r;
	}
}


IC void q_scalem(Fmatrix& m, float v)
{
	Fquaternion q;
	q.set(m);
	q_scale(q, v);
	m.rotation(q);
}


//sclale base' * q by scale_factor returns result in matrix  m_res
IC void q_scale_vs_basem(Fmatrix& m_res, const Fquaternion& q, const Fquaternion& base, float scale_factor)
{
	Fmatrix mb, imb;
	mb.rotation(base);
	imb.invert(mb);

	Fmatrix m;
	m.rotation(q);
	m_res.mul(imb, m);
	q_scalem(m_res, scale_factor);
}


IC void q_add_scaled_basem(Fquaternion& q, const Fquaternion& base, const Fquaternion& q0, const Fquaternion& q1,
                           float v1)
{
	//VERIFY(0.f =< v && 1.f >= v );
	Fmatrix m0;
	m0.rotation(q0);
	Fmatrix m, ml1;
	q_scale_vs_basem(ml1, q1, base, v1);
	m.mul(m0, ml1);
	q.set(m);
	q.normalize();
}

IC float DET(const Fmatrix& a)
{
	return
	((a._11 * (a._22 * a._33 - a._23 * a._32) -
		a._12 * (a._21 * a._33 - a._23 * a._31) +
		a._13 * (a._21 * a._32 - a._22 * a._31)));
}

IC bool check_scale(const Fmatrix& m)
{
	float det = DET(m);
	return (0.8f < det && det < 1.3f);
}

IC bool check_scale(const Fquaternion& q)
{
	Fmatrix m;
	m.rotation(q);
	return check_scale(m);
}

IC void MixFactors(float* F, int b_count)
{
	float sum = 0;
	for (int i = 0; i < b_count; i++)
		sum += F[i];
	for (int i2 = 0; i2 < b_count; i2++)
		F[i2] /= sum;
}

IC void MixinAdd(CKey& Result, const CKey* R, const float* BA, int b_count)
{
	for (int i = 0; i < b_count; i++)
		key_mad(Result, Result, R[i], BA[i]);
}

IC void MixAdd(CKey& Result, const CKey* R, const float* BA, int b_count)
{
	key_identity(Result);
	MixinAdd(Result, R, BA, b_count);
}

IC void process_single_channel(CKey& Result, const animation::channel_def& ch, const CKey* R,
                               const CBlend* const BA[MAX_BLENDED], int b_count)
{
	MixInterlerp(Result, R, BA, b_count);
	VERIFY(_valid( Result.T ));
	VERIFY(_valid( Result.Q ));
}

IC void MixChannels(CKey& Result, const CKey* R, const animation::channel_def* BA, int b_count)
{
	VERIFY(b_count > 0 && b_count <= MAX_CHANNELS);
	SMotionChannelDef channels[MAX_CHANNELS];
	for (int i = 0; i < b_count; ++i)
	{
		channels[i].factor = BA[i].factor;
		switch (BA[i].rule.extern_)
		{
		case animation::add:
			channels[i].mix = eMotionChannelAdd;
			break;
		case animation::lerp:
			channels[i].mix = eMotionChannelLerp;
			break;
		default:
			NODEFAULT;
		}
	}
	MixMotionChannels(Result, R, channels, b_count);
	VERIFY(_valid( Result.T ));
	VERIFY(_valid( Result.Q ));
}
