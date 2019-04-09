
static const float PI = 3.14159265359;

TextureCube gCubeMap		: register(t0);
SamplerState basicSampler	: register(s0);

cbuffer cbPerObject : register(b0)
{
	float4x4 gWorld;
	float4x4 gTexTransform;
	uint gMaterialIndex;
	uint gObjPad0;
	uint gObjPad1;
	uint gObjPad2;
};

// Constant data that varies per material.
cbuffer cbPass : register(b1)
{
	float4x4 gViewProj;
	float3 gEyePosW;
	float roughnessCb;
	/*
	float4x4 gView;
	float4x4 gInvView;
	float4x4 gProj;
	float4x4 gInvProj;
	float4x4 gViewProj;
	float4x4 gInvViewProj;
	float4x4 gViewProjTex;
	float4x4 gShadowTransform;
	float3 gEyePosW;
	float cbPerObjectPad1;
	float2 gRenderTargetSize;
	float2 gInvRenderTargetSize;
	float gNearZ;
	float gFarZ;
	float gTotalTime;
	float gDeltaTime;
	float4 gAmbientLight;
	*/
	// Indices [0, NUM_DIR_LIGHTS) are directional lights;
	// indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
	// indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
	// are spot lights for a maximum of MaxLights per object.
	//Light gLights[MaxLights];
};

float RadicalInverse_VdC(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

float2 Hammersley(uint i, uint N)
{
	return float2(float(i) / float(N), RadicalInverse_VdC(i));
}

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
	float a = roughness * roughness;

	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

	// from spherical coordinates to cartesian coordinates
	float3 H;
	H.x = cos(phi) * sinTheta;
	H.y = sin(phi) * sinTheta;
	H.z = cosTheta;

	// from tangent-space vector to world-space sample vector
	float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 tangent = normalize(cross(up, N));
	float3 bitangent = cross(N, tangent);

	float3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sampleVec);
}

struct VertexOut
{
	float4 PosH		: SV_POSITION;
	float3 PosL		: POSITION;
};

float4 main(VertexOut pin) : SV_TARGET
{
	/*
	float roughness = 0.1f;
	const uint NumSamples = 1024u;
	float3 N = normalize(input.uvw);
	float3 R = N;
	float3 V = R;

	float3 prefilteredColor = float3(0.f, 0.f, 0.f);
	float totalWeight = 0.f;
	for (uint i = 0u; i < NumSamples; ++i)
	{
		float2 Xi = Hammersley(i, NumSamples);
		float3 H = ImportanceSampleGGX(Xi, N, roughness);
		float3 L = normalize(2.0f * dot(V, H) * H - V);
		float NdotL = max(dot(N, L), 0.0f);
		if (NdotL > 0.f)
		{
			prefilteredColor += EnvMap.Sample(basicSampler, L).rgb * NdotL;
			totalWeight += NdotL;
		}
	}

	prefilteredColor = prefilteredColor / totalWeight;

	return float4(prefilteredColor, 1);
	*/

	// Pre-integration
	float3 irradiance = float3(0.0f, 0.0f, 0.0f);

	float3 normal = normalize(pin.PosL);
	float3 up = float3(0.0, 1.0, 0.0);
	float3 right = cross(up, normal);
	up = cross(normal, right);

	float sampleDelta = 0.025f;
	//sampleDelta = 0.2f;
	float numSamples = 0.0f;
	for (float phi = 0.0f; phi < 2.0f * PI; phi += sampleDelta)
	{
		for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
		{
			// spherical to cartesian (in tangent space)
			float3 tangentSample = float3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
			// tangent space to world
			float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal;
			
			irradiance += gCubeMap.Sample(basicSampler, sampleVec).rgb * cos(theta) * sin(theta);
			numSamples++;
		}
	}
	irradiance = PI * irradiance * (1.0f / numSamples);

	return float4(irradiance, 1.0f);
	//return float4(1.0f, 1.0f, 1.0f, 1.0f);
}