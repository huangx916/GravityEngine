
#ifndef _SHADER_DEFINITION_H
#define _SHADER_DEFINITION_H


//----------------------------------------------------------------------------------------------------------
// Reverse Z
//----------------------------------------------------------------------------------------------------------
#define USE_REVERSE_Z 1

#define Z_UPPER_BOUND 50000.0f
#define Z_LOWER_BOUND 1.0f
#define Z_UPPER_BOUND_NORM 1.0f
#define Z_LOWER_BOUND_NORM 0.0f

#if USE_REVERSE_Z

#define FAR_Z Z_LOWER_BOUND
#define NEAR_Z Z_UPPER_BOUND
#define FAR_Z_NORM Z_LOWER_BOUND_NORM
#define NEAR_Z_NORM Z_UPPER_BOUND_NORM

#else

#define FAR_Z Z_UPPER_BOUND
#define NEAR_Z Z_LOWER_BOUND
#define FAR_Z_NORM Z_UPPER_BOUND_NORM
#define NEAR_Z_NORM Z_LOWER_BOUND_NORM

#endif


//----------------------------------------------------------------------------------------------------------
// Depth readback
//----------------------------------------------------------------------------------------------------------
#define DEPTH_READBACK_BUFFER_SIZE_X 256
#define DEPTH_READBACK_BUFFER_SIZE_Y 128

#define DEPTH_READBACK_BUFFER_SIZE (DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y)

#define DEPTH_DOWNSAMPLE_THREAD_NUM_X 8
#define DEPTH_DOWNSAMPLE_THREAD_NUM_Y 8


//----------------------------------------------------------------------------------------------------------
// TBDR
//----------------------------------------------------------------------------------------------------------
#define TILE_SIZE_X 16
#define TILE_SIZE_Y 16

#define TILE_THREAD_NUM_X 8
#define TILE_THREAD_NUM_Y 8

#define COMPUTE_SHADER_TILE_GROUP_SIZE (TILE_THREAD_NUM_X * TILE_THREAD_NUM_Y)


//----------------------------------------------------------------------------------------------------------
// CBDR
//----------------------------------------------------------------------------------------------------------
#define CLUSTER_SIZE_X 64
#define CLUSTER_SIZE_Y 64
#define CLUSTER_NUM_Z 16

#define CLUSTER_THREAD_NUM_X 8
#define CLUSTER_THREAD_NUM_Y 8

#define COMPUTE_SHADER_CLUSTER_GROUP_SIZE (CLUSTER_THREAD_NUM_X * CLUSTER_THREAD_NUM_Y)


static const float DepthSlicing_16[17] = {
	1.0f, 20.0f, 29.7f, 44.0f, 65.3f,
	96.9f, 143.7f, 213.2f, 316.2f, 469.1f,
	695.9f, 1032.4f, 1531.5f, 2272.0f, 3370.5f,
	5000.0f, 50000.0f
};


//----------------------------------------------------------------------------------------------------------
// Light
//----------------------------------------------------------------------------------------------------------
#define MAX_GRID_POINT_LIGHT_NUM 80
#define MAX_GRID_SPOTLIGHT_NUM 20

 



#endif 