// The city camera's air (docs/cs2_look_pass.md), shared by post.hlsl (CityComposite, over the lit scene) and water.hlsl (AirTo,
// per water vertex), so the sea fades into the same haze as the land. Selected by PostParams misc.z = 0.5 (FrameView::cityAir).
// Closed form, no textures and no marching: every preset down to an iPhone 15 runs it. A pale sky gradient from the CPU's horizon
// and zenith radiance, and exponential height fog whose optical depth along the view ray is integrated exactly (Quilez, "Better
// Fog"). Fully fogged ground takes the horizon's own colour, so terrain melts into the sky without a line, as Cities: Skylines II's
// aerial views do. Fitted to its screenshots: about half-way to the haze at 2 km along the ground, yet a low layer, so from 5 km up
// the city keeps its blacks (CS2's 1% is 17 of 255). Inputs are passed in: the two files name their constant buffers differently.
#ifndef TBD_CITY_AIR_HLSLI
#define TBD_CITY_AIR_HLSLI

static const float kCityFog = 4.8e-4, kCityFogHeight = 250.0, kCityFogStart = 250.0; // extinction per metre at sea level; its scale height; clear air in front of the camera

bool CityAirOn(float miscZ) { return miscZ > 0.25 && miscZ < 0.75; }

// By day the sky sits well above the ground, as in CS2. With the sun low the ground goes dark, auto exposure opens up, and the
// same gain would burn the horizon white and lose its orange: so it falls off toward sunset.
float CitySkyGain(float sunUp) { return lerp(0.9, 1.95, saturate(sunUp * 4.0)); }

// The haze is the horizon's colour, so it pales toward blue-grey with the sky by day; at dusk the horizon keeps its orange and rose.
// Brighter than the ground, as CS2's horizon is.
float3 CityHorizonOf(float3 horizonRadiance, float sunUp) {
    float l = dot(horizonRadiance, float3(0.2126, 0.7152, 0.0722));
    float keep = lerp(0.85, 0.5, saturate(sunUp * 4.0));
    return lerp(l.xxx, horizonRadiance, keep) * CitySkyGain(sunUp);
}

// Forward scattering round the sun: a wide warm bloom and a tighter core.
float3 CitySunGlowOf(float3 sunColor, float mu) {
    float m = saturate(mu);
    return sunColor * (0.05 * m * m * m + 0.35 * pow(m, 24.0));
}

// A clean blue overhead (CS2's street shots), paling to the horizon.
float3 CitySkyOf(float3 d, float3 zenithRadiance, float3 horizonRadiance, float3 sunColor, float3 toSun) {
    float3 zen = lerp(dot(zenithRadiance, float3(0.2126, 0.7152, 0.0722)).xxx, zenithRadiance, 0.9) * CitySkyGain(toSun.y) * 1.08;
    float3 sky = lerp(CityHorizonOf(horizonRadiance, toSun.y), zen, pow(saturate(d.y), 0.45));
    return sky + CitySunGlowOf(sunColor, dot(d, toSun));
}

// How much of a surface's light reaches the eye through the haze (1 clear .. 0 all haze).
float CityAirKeep(float3 eyePos, float3 world) {
    float dist = length(world - eyePos);
    float ye = max(eyePos.y, 0.0), yw = max(world.y, 0.0), dy = yw - ye;
    float k = 1.0 / kCityFogHeight, base = exp(-ye * k);
    float od = kCityFog * max(dist - kCityFogStart, 0.0) * (abs(dy) > 1.0 ? (base - exp(-yw * k)) / (dy * k) : base);
    return exp(-od);
}

// The haze's own light along the view direction d.
float3 CityHazeOf(float3 d, float3 horizonRadiance, float3 sunColor, float3 toSun) {
    return CityHorizonOf(horizonRadiance, toSun.y) + CitySunGlowOf(sunColor, dot(d, toSun));
}

#endif
