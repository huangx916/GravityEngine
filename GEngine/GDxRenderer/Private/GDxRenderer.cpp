#include "stdafx.h"
#include "GDxRenderer.h"
#include "GDxRendererFactory.h"
#include "GDxTexture.h"
#include "GDxFloat4.h"
#include "GDxFloat4x4.h"
#include "GDxFilmboxManager.h"
#include "GDxMesh.h"
#include "GDxSceneObject.h"
#include "GDxInputLayout.h"
#include "GDxShaderManager.h"
#include "GDxStaticVIBuffer.h"

#include <WindowsX.h>

using Microsoft::WRL::ComPtr;
using namespace std;
using namespace DirectX;


#pragma region Class

GDxRenderer& GDxRenderer::GetRenderer()
{
	static GDxRenderer *instance = new GDxRenderer();
	return *instance;
}

GDxRenderer::GDxRenderer()
{
	// Estimate the scene bounding sphere manually since we know how the scene was constructed.
	// The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
	// the world space origin.  In general, you need to loop over every world space vertex
	// position and compute the bounding sphere.
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(10.0f*10.0f + 15.0f*15.0f);
}

GDxRenderer::~GDxRenderer()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

#pragma endregion

#pragma region Main

bool GDxRenderer::InitDirect3D()
{
	ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory)));

	// Try to create hardware device.
	HRESULT hardwareResult = D3D12CreateDevice(
		nullptr,             // default adapter
		D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&md3dDevice));

	// Fallback to WARP device.
	if (FAILED(hardwareResult))
	{
		ComPtr<IDXGIAdapter> pWarpAdapter;
		ThrowIfFailed(mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			pWarpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&md3dDevice)));
	}

	ThrowIfFailed(md3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&mFence)));

	mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Check 4X MSAA quality support for our back buffer format.
	// All Direct3D 11 capable devices support 4X MSAA for all render 
	// target formats, so we only need to check quality support.

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = mBackBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	ThrowIfFailed(md3dDevice->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels)));

	//m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	//assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");

#ifdef _DEBUG
	LogAdapters();
#endif

	CreateCommandObjects();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();

	return true;
}

void GDxRenderer::PreInitialize(HWND OutputWindow, double width, double height)
{
	mhMainWnd = OutputWindow;
	mClientWidth = (int)width;
	mClientHeight = (int)height;

	if (!InitDirect3D())
		return;

	CreateRendererFactory();
	CreateFilmboxManager();

	// Do the initial resize code.
	OnResize();

	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));
}

void GDxRenderer::Initialize()
{
	auto numThreads = thread::hardware_concurrency();
	mRendererThreadPool = std::make_unique<GGiThreadPool>(numThreads);

	InitializeGpuProfiler();
	BuildDescriptorHeaps();
	BuildRootSignature();
	BuildFrameResources();
	BuildPSOs();

	/*
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists2[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists2), cmdsLists2);
	*/

	// Wait until initialization is complete.
	FlushCommandQueue();

#ifdef USE_IMGUI
	pImgui->Initialize(MainWnd(), md3dDevice.Get(), NUM_FRAME_RESOURCES, mSrvDescriptorHeap.Get());
#endif

	/*
	mCurrFrameResource = mFrameResources[0].get();

	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));
	*/

	CubemapPreIntegration();

	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until initialization is complete.
	FlushCommandQueue();

	GRiOcclusionCullingRasterizer::GetInstance().Init(
		DEPTH_READBACK_BUFFER_SIZE_X,
		DEPTH_READBACK_BUFFER_SIZE_Y,
		Z_LOWER_BOUND,
		Z_UPPER_BOUND,
#if USE_REVERSE_Z
		true
#else
		false
#endif
	);

	BuildMeshSDF();
}

void GDxRenderer::Draw(const GGiGameTimer* gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["GBuffer"].Get()));

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	GDxGpuProfiler::GetGpuProfiler().BeginFrame();

	auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear the back buffer.
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);

	// Clear depth buffer.
#if USE_REVERSE_Z
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
#else
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
#endif

	// G-Buffer Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("G-Buffer Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GBuffer"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GBuffer"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GBuffer"].Get());

		mCommandList->SetPipelineState(mPSOs["GBuffer"].Get());

		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

		//mCommandList->SetGraphicsRootDescriptorTable(2, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrv(mTextrueHeapIndex));

		matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(4, matBuffer->GetGPUVirtualAddress());

		mCommandList->OMSetStencilRef(1);

		// Indicate a state transition on the resource usage.
		for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		{
			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[i]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			// Clear the back buffer.
			DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[0],
			mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[1],
			mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[2],
			mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[3]
			};

			// WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
			// SO DO NOT CLEAR DEPTH.
			mCommandList->ClearRenderTargetView(mRtvHeaps["GBuffer"]->mRtvHeap.handleCPU((UINT)i), clearColor, 0, nullptr);
		}

		// Specify the buffers we are going to render to.
		//mCommandList->OMSetRenderTargets(mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, &(mRtvHeaps["GBuffer"]->mRtvHeap.hCPUHeapStart), true, &DepthStencilView());
		mCommandList->OMSetRenderTargets(mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, &(mRtvHeaps["GBuffer"]->mRtvHeap.hCPUHeapStart), true, &DepthStencilView());

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Deferred, true, true, true);

		for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		{
			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[i]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("G-Buffer Pass");
	}

	// Depth Downsample Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Depth Downsample Pass");

		mCommandList->SetComputeRootSignature(mRootSignatures["DepthDownsamplePass"].Get());

		mCommandList->SetPipelineState(mPSOs["DepthDownsamplePass"].Get());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["DepthDownsamplePass"]->GetResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetComputeRootDescriptorTable(1, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetComputeRootDescriptorTable(2, mUavs["DepthDownsamplePass"]->GetGpuUav());

		UINT numGroupsX = (UINT)(DEPTH_READBACK_BUFFER_SIZE_X / DEPTH_DOWNSAMPLE_THREAD_NUM_X);
		UINT numGroupsY = (UINT)(DEPTH_READBACK_BUFFER_SIZE_Y / DEPTH_DOWNSAMPLE_THREAD_NUM_Y);

		mCommandList->Dispatch(numGroupsX, numGroupsY, 1);

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["DepthDownsamplePass"]->GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));

		mCommandList->CopyResource(mDepthReadbackBuffer.Get(), mUavs["DepthDownsamplePass"]->GetResource());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["DepthDownsamplePass"]->GetResource(),
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Depth Downsample Pass");
	}

	// Tile/Cluster Pass
	{
#if USE_TBDR
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Tile Pass");
#elif USE_CBDR
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Cluster Pass");
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif

		mCommandList->RSSetViewports(1, &(mUavs["TileClusterPass"]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mUavs["TileClusterPass"]->mScissorRect));

		mCommandList->SetComputeRootSignature(mRootSignatures["TileClusterPass"].Get());

		mCommandList->SetPipelineState(mPSOs["TileClusterPass"].Get());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["TileClusterPass"]->GetResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		auto lightCB = mCurrFrameResource->LightCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(0, lightCB->GetGPUVirtualAddress());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetComputeRootDescriptorTable(2, mUavs["TileClusterPass"]->GetGpuUav());

		mCommandList->SetComputeRootDescriptorTable(3, GetGpuSrv(mDepthBufferSrvIndex));

#if USE_TBDR
		UINT numGroupsX = (UINT)ceilf((float)mClientWidth / TILE_SIZE_X);
		UINT numGroupsY = (UINT)ceilf((float)mClientHeight / TILE_SIZE_Y);
		UINT numGroupsZ = 1;
#elif USE_CBDR
		UINT numGroupsX = (UINT)ceilf((float)mClientWidth / CLUSTER_SIZE_X);
		UINT numGroupsY = (UINT)ceilf((float)mClientHeight / CLUSTER_SIZE_Y);
		UINT numGroupsZ = 1;
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif
		mCommandList->Dispatch(numGroupsX, numGroupsY, numGroupsZ);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["TileClusterPass"]->GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

#if USE_TBDR
		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Tile Pass");
#elif USE_CBDR
		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Cluster Pass");
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif
	}

	// Screen Space Shadow Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Screen Space Shadow Pass");

		//ID3D12DescriptorHeap* sdfSrvDescriptorHeaps[] = { mSdfSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(sdfSrvDescriptorHeaps), sdfSrvDescriptorHeaps);

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["ScreenSpaceShadowPass"].Get());

		mCommandList->SetPipelineState(mPSOs["ScreenSpaceShadowPass"].Get());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->SetGraphicsRoot32BitConstant(0, mSceneObjectSdfNum, 0);

		auto meshSdfDesBuffer = mMeshSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(1, meshSdfDesBuffer->GetGPUVirtualAddress());

		auto soSdfDesBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, soSdfDesBuffer->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mSdfTextrueIndex));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(5, passCB->GetGPUVirtualAddress());

		mCommandList->OMSetRenderTargets(1, &mRtvHeaps["ScreenSpaceShadowPass"]->mRtvHeap.handleCPU(0), false, nullptr);

		// Clear the render target.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[0],
		mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[1],
		mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[2],
		mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["ScreenSpaceShadowPass"]->mRtvHeap.handleCPU(0), clearColor, 0, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		//ID3D12DescriptorHeap* srvDescriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(srvDescriptorHeaps), srvDescriptorHeaps);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Screen Space Shadow Pass");
	}

	// Light Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Light Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["LightPass"].Get());

		mCommandList->SetPipelineState(mPSOs["LightPass"].Get());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		auto lightCB = mCurrFrameResource->LightCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, lightCB->GetGPUVirtualAddress());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mUavs["TileClusterPass"]->GetGpuSrv());

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpuStart());

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(5, mRtvHeaps["ScreenSpaceShadowPass"]->GetSrvGpuStart());

		mCommandList->SetGraphicsRootDescriptorTable(6, GetGpuSrv(mIblIndex));

		mCommandList->OMSetRenderTargets(1, &mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(0), false, nullptr);

		// Clear the render target.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[0],
		mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[1],
		mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[2],
		mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(0), clearColor, 0, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Light Pass");
	}

	// Sky Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Sky Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["Sky"].Get());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[mVelocityBufferSrvIndex - mGBufferSrvIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		D3D12_CPU_DESCRIPTOR_HANDLE skyRtvs[2] =
		{
			mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(0),
			mRtvHeaps["GBuffer"]->mRtvHeap.handleCPU(mVelocityBufferSrvIndex - mGBufferSrvIndex)
		};
		mCommandList->OMSetRenderTargets(2, skyRtvs, false, &DepthStencilView());

		auto passCB = mCurrFrameResource->SkyCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		// Sky cubemap SRV.
		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mSkyTexHeapIndex));
		//mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mIblIndex + 4)); //Irradiance cubemap debug.

		mCommandList->SetPipelineState(mPSOs["Sky"].Get());
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Sky, true, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[mVelocityBufferSrvIndex - mGBufferSrvIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Sky Pass");
	}

	// TAA Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("TAA Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["TaaPass"]->mRtv[2]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["TaaPass"]->mRtv[2]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["TaaPass"].Get());

		mCommandList->SetPipelineState(mPSOs["TaaPass"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["LightPass"]->GetSrvGpu(0));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["TaaPass"]->GetSrvGpu(mTaaHistoryIndex));
		mTaaHistoryIndex = (mTaaHistoryIndex + 1) % 2;

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mVelocityBufferSrvIndex - mGBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[2]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 taaClearColor = { mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[0],
		mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[1],
		mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[2],
		mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(2), taaClearColor, 0, nullptr);

		DirectX::XMVECTORF32 hisClearColor = { mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[0],
		mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[1],
		mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[2],
		mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(mTaaHistoryIndex), hisClearColor, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE taaRtvs[2] =
		{
			mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(2),
			mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(mTaaHistoryIndex)
		};
		//mCommandList->OMSetRenderTargets(2, taaRtvs, false, &DepthStencilView());
		mCommandList->OMSetRenderTargets(2, taaRtvs, false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[2]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("TAA Pass");
	}

	// Motion Blur Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Motion Blur Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlurPass"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlurPass"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurPass"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurPass"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["TaaPass"]->GetSrvGpu(2));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["GBuffer"]->GetSrvGpu(mVelocityBufferSrvIndex - mGBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlurPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlurPass"]->mRtv[0]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlurPass"]->mRtv[0]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlurPass"]->mRtv[0]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlurPass"]->mRtv[0]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlurPass"]->mRtvHeap.handleCPU(0), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlurPass"]->mRtvHeap.handleCPU(0)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlurPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Motion Blur Pass");
	}

	// Post Process Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Post Processing Pass");

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["PostProcess"].Get());

		mCommandList->SetPipelineState(mPSOs["PostProcess"].Get());

		mCommandList->SetGraphicsRootDescriptorTable(0, mRtvHeaps["MotionBlurPass"]->GetSrvGpu(0));
		//mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["SkyPass"]->GetSrvGpuStart());

		// Specify the buffers we are going to render to.
		//mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Post Processing Pass");
	}

	// Debug Pass
#if 0
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Debug Pass");

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GBufferDebug"].Get());

		mCommandList->SetPipelineState(mPSOs["GBufferDebug"].Get());

		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpuStart());

		matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(4, matBuffer->GetGPUVirtualAddress());

		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Debug, true, true);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Debug Pass");
	}
#endif

	// SDF Debug Pass
#if 0
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SDF Debug Pass");

		//ID3D12DescriptorHeap* sdfSrvDescriptorHeaps[] = { mSdfSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(sdfSrvDescriptorHeaps), sdfSrvDescriptorHeaps);

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SdfDebug"].Get());

		mCommandList->SetPipelineState(mPSOs["SdfDebug"].Get());

		mCommandList->SetGraphicsRoot32BitConstant(0, mSceneObjectSdfNum, 0);

		auto meshSdfDesBuffer = mMeshSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(1, meshSdfDesBuffer->GetGPUVirtualAddress());

		auto soSdfDesBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, soSdfDesBuffer->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrv(mSdfTextrueIndex));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(4, passCB->GetGPUVirtualAddress());

		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		//ID3D12DescriptorHeap* srvDescriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(srvDescriptorHeaps), srvDescriptorHeaps);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SDF Debug Pass");
	}
#endif

	// Immediate Mode GUI Pass
	{

#ifdef USE_IMGUI

		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GUI Pass");

		pImgui->Render(mCommandList.Get());

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GUI Pass");

#endif

	}

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Swap the back and front buffers
	//ThrowIfFailed(mSwapChain->Present(1, 0)); // Present with vsync
	ThrowIfFailed(mSwapChain->Present(0, 0)); // Present without vsync
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	// Advance the fence value to mark commands up to this fence point.
	mCurrFrameResource->Fence = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point. 
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);

	GDxGpuProfiler::GetGpuProfiler().EndFrame();
}

void GDxRenderer::Update(const GGiGameTimer* gt)
{
	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % NUM_FRAME_RESOURCES;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	//
	// Animate the lights (and hence shadows).
	//

	mLightRotationAngle += 0.1f * gt->DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

	GGiCpuProfiler::GetInstance().StartCpuProfile("Cpu Update Constant Buffers");

	ScriptUpdate(gt);

	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateSdfDescriptorBuffer(gt);
	UpdateShadowTransform(gt);
	UpdateMainPassCB(gt);
	UpdateSkyPassCB(gt);
	UpdateLightCB(gt);

	GGiCpuProfiler::GetInstance().EndCpuProfile("Cpu Update Constant Buffers");

	CullSceneObjects(gt);
}

void GDxRenderer::OnResize()
{
	assert(md3dDevice);
	assert(mSwapChain);
	assert(mDirectCmdListAlloc);

	// Flush before changing any resources.
	FlushCommandQueue();

	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	// Release the previous resources we will be recreating.
	for (int i = 0; i < SwapChainBufferCount; ++i)
		mSwapChainBuffer[i].Reset();
	mDepthStencilBuffer.Reset();

	// Resize the swap chain.
	ThrowIfFailed(mSwapChain->ResizeBuffers(
		SwapChainBufferCount,
		mClientWidth, mClientHeight,
		mBackBufferFormat,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

	mCurrBackBuffer = 0;

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < SwapChainBufferCount; i++)
	{
		ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i])));
		md3dDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);
		rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	}

	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = mClientWidth;
	depthStencilDesc.Height = mClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS; //DXGI_FORMAT_R24G8_TYPELESS;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
	dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
	dsv_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; //DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsv_desc.Texture2D.MipSlice = 0;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; //DXGI_FORMAT_D24_UNORM_S8_UINT;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(mDepthStencilBuffer.GetAddressOf())));

	if (mDepthBufferSrvIndex != 0)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; //DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &srvDesc, GetCpuSrv(mDepthBufferSrvIndex));
	}

	// Create descriptor to mip level 0 of entire resource using the format of the resource.
	md3dDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), &dsv_desc, DepthStencilView());

	// Create depth readback buffer
	{
		// Free the old resources if they exist.
		if (mDepthReadbackBuffer != nullptr)
			mDepthReadbackBuffer.Reset();

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(DEPTH_READBACK_BUFFER_SIZE * 4),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(mDepthReadbackBuffer.GetAddressOf())));
	}

	// Transition the resource from its initial state to be used as a depth buffer.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	// Execute the resize commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until resize is complete.
	FlushCommandQueue();

	// Update the viewport transform to cover the client area.
	mScreenViewport.TopLeftX = 0;
	mScreenViewport.TopLeftY = 0;
	mScreenViewport.Width = static_cast<float>(mClientWidth);
	mScreenViewport.Height = static_cast<float>(mClientHeight);
	mScreenViewport.MinDepth = 0.0f;
	mScreenViewport.MaxDepth = 1.0f;

	mScissorRect = { 0, 0, mClientWidth, mClientHeight };

	for (auto &rtvHeap : mRtvHeaps)
	{
		if (rtvHeap.second->mRtv.size() != 0 && rtvHeap.second->mRtv[0]->mResource != nullptr)
		{
			rtvHeap.second->OnResize(mClientWidth, mClientHeight);
		}
	}

	for (auto &uav : mUavs)
	{
		if (uav.second->ResourceNotNull())
		{
			if (uav.second->IsTexture())
				uav.second->OnResize(mClientWidth, mClientHeight);
			else
			{
				if (uav.first == "TileClusterPass")
				{
					UINT64 elementNum = 0;
#if USE_TBDR
					elementNum = UINT64((float)ceilf(mClientWidth / (float)TILE_SIZE_X) * ceilf((float)mClientHeight / (float)TILE_SIZE_Y) + 0.01);
#elif USE_CBDR
					elementNum = (UINT64)(ceilf((float)mClientWidth / (float)CLUSTER_SIZE_X) * ceilf((float)mClientHeight / (float)CLUSTER_SIZE_Y) * CLUSTER_NUM_Z + 0.01);
#else
					ThrowGGiException("TBDR/CBDR not enabled");
#endif
					uav.second->OnBufferResize((UINT)elementNum);
				}
			}
		}
	}
}

#pragma endregion

#pragma region Update

void GDxRenderer::ScriptUpdate(const GGiGameTimer* gt)
{
	/*
	if (pSceneObjects.find(L"MoveSphere") != pSceneObjects.end())
	{
		std::vector<float> loc = pSceneObjects[L"MoveSphere"]->GetLocation();
		pSceneObjects[L"MoveSphere"]->SetLocation(70 * DirectX::XMScalarSin(gt->TotalTime()), loc[1], loc[2]);
	}
	*/

	//auto test1 = GGiFloat4x4::Identity();
	//auto test2 = GGiFloat4x4::Identity();
	//auto test3 = GGiFloat4x4::Identity();
	//auto test4 = test1 * test2;
	//auto test5 = test4 * test3;
	//test5 = test5;
}

void GDxRenderer::UpdateObjectCBs(const GGiGameTimer* gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : pSceneObjects)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if (e.second->NumFramesDirty > 0)
		{
			e.second->UpdateTransform();

			/*
			auto dxTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTransform());
			if (dxTrans == nullptr)
				ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");

			auto dxPrevTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetPrevTransform());
			if (dxPrevTrans == nullptr)
				ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");

			auto dxTexTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTexTransform());
			if (dxTexTrans == nullptr)
				ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");
			*/
			
			//XMMATRIX renderObjectTrans = XMLoadFloat4x4(&(dxTrans->GetValue())); 
			XMMATRIX renderObjectTrans = GDx::GGiToDxMatrix(e.second->GetTransform());
			XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(renderObjectTrans), renderObjectTrans);
			XMMATRIX invTransWorld = XMMatrixTranspose(invWorld);
			//XMMATRIX prevWorld = XMLoadFloat4x4(&(dxPrevTrans->GetValue()));
			XMMATRIX prevWorld = GDx::GGiToDxMatrix(e.second->GetPrevTransform());
			//auto tempSubTrans = e->GetSubmesh().Transform;
			//XMMATRIX submeshTrans = XMLoadFloat4x4(&tempSubTrans);
			//XMMATRIX texTransform = XMLoadFloat4x4(&(dxTexTrans->GetValue()));
			XMMATRIX texTransform = GDx::GGiToDxMatrix(e.second->GetTexTransform());
			//auto world = submeshTrans * renderObjectTrans;
			auto world = renderObjectTrans;

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.PrevWorld, XMMatrixTranspose(prevWorld));
			XMStoreFloat4x4(&objConstants.InvTransWorld, XMMatrixTranspose(invTransWorld));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			/*
			if (e.second->GetMesh()->NumFramesDirty > 0)
			{
				objConstants.MaterialIndex = e.second->GetMaterial()->MatIndex;
			}
			*/

			currObjectCB->CopyData(e.second->GetObjIndex(), objConstants);

			// Next FrameResource need to be updated too.
			e.second->NumFramesDirty--;
		}
	}
}

void GDxRenderer::UpdateLightCB(const GGiGameTimer* gt)
{
	LightConstants lightCB;

	auto pos = pCamera->GetPosition();
	lightCB.cameraPosition = DirectX::XMFLOAT3(pos[0], pos[1], pos[2]);

	lightCB.dirLight[0].Direction[0] = 0.57735f;
	lightCB.dirLight[0].Direction[1] = -0.57735f;
	lightCB.dirLight[0].Direction[2] = -0.57735f;
	lightCB.dirLight[0].DiffuseColor[0] = 0.7f;
	lightCB.dirLight[0].DiffuseColor[1] = 0.7f;
	lightCB.dirLight[0].DiffuseColor[2] = 0.6f;
	lightCB.dirLight[0].DiffuseColor[3] = 1.0f;
	lightCB.dirLight[0].AmbientColor[0] = 0.0f;
	lightCB.dirLight[0].AmbientColor[1] = 0.0f;
	lightCB.dirLight[0].AmbientColor[2] = 0.0f;
	lightCB.dirLight[0].AmbientColor[3] = 1.0f;
	lightCB.dirLight[0].Intensity = 3.0f;

	lightCB.dirLight[1].Direction[0] = -0.57735f;
	lightCB.dirLight[1].Direction[1] = -0.57735f;
	lightCB.dirLight[1].Direction[2] = -0.57735f;
	lightCB.dirLight[1].DiffuseColor[0] = 0.6f;
	lightCB.dirLight[1].DiffuseColor[1] = 0.6f;
	lightCB.dirLight[1].DiffuseColor[2] = 0.6f;
	lightCB.dirLight[1].DiffuseColor[3] = 1.0f;
	lightCB.dirLight[1].AmbientColor[0] = 0.0f;
	lightCB.dirLight[1].AmbientColor[1] = 0.0f;
	lightCB.dirLight[1].AmbientColor[2] = 0.0f;
	lightCB.dirLight[1].AmbientColor[3] = 1.0f;
	lightCB.dirLight[1].Intensity = 3.0f;

	lightCB.dirLight[2].Direction[0] = 0.0;
	lightCB.dirLight[2].Direction[1] = -0.707f;
	lightCB.dirLight[2].Direction[2] = 0.707f;
	lightCB.dirLight[2].DiffuseColor[0] = 0.5f;
	lightCB.dirLight[2].DiffuseColor[1] = 0.5f;
	lightCB.dirLight[2].DiffuseColor[2] = 0.5f;
	lightCB.dirLight[2].DiffuseColor[3] = 1.0f;
	lightCB.dirLight[2].AmbientColor[0] = 0.0f;
	lightCB.dirLight[2].AmbientColor[1] = 0.0f;
	lightCB.dirLight[2].AmbientColor[2] = 0.0f;
	lightCB.dirLight[2].AmbientColor[3] = 1.0f;
	lightCB.dirLight[2].Intensity = 3.0f;

	lightCB.pointLight[0].Color[0] = 1.0f;
	lightCB.pointLight[0].Color[1] = 1.0f;
	lightCB.pointLight[0].Color[2] = 1.0f;
	lightCB.pointLight[0].Color[3] = 1.0f;
	lightCB.pointLight[0].Intensity = 100.0f;
	lightCB.pointLight[0].Position[0] = 0.0f;
	lightCB.pointLight[0].Position[1] = -4.0f;
	lightCB.pointLight[0].Position[2] = 0.0f;
	lightCB.pointLight[0].Range = 100.0f;

	/*
	int lightCount = 16;
	for (int i = -lightCount; i < lightCount; i++)
	{
		for (int j = -lightCount; j < lightCount; j++)
		{
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[0] = ((abs(i * j + 1) % 8) * 0.1f + 0.2f);
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[1] = ((abs(i * j + 2) % 7) * 0.1f + 0.3f);
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[2] = ((abs(i * j) % 6) * 0.1f + 0.4f);
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[3] = 1.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Intensity = 500.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Position[0] = i * 30.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Position[1] = -15.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Position[2] = j * 30.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Range = 50.0f;
		}
	}
	*/

	lightCB.pointLight[0].Color[0] = 1.0f;
	lightCB.pointLight[0].Color[1] = 1.0f;
	lightCB.pointLight[0].Color[2] = 1.0f;
	lightCB.pointLight[0].Color[3] = 1.0f;
	lightCB.pointLight[0].Intensity = 5000.0f;
	lightCB.pointLight[0].Position[0] = 0.0f;
	lightCB.pointLight[0].Position[1] = 0.0f;
	lightCB.pointLight[0].Position[2] = 0.0f;
	lightCB.pointLight[0].Range = 50000.0f;

	lightCB.pointLight[1].Color[0] = 1.0f;
	lightCB.pointLight[1].Color[1] = 1.0f;
	lightCB.pointLight[1].Color[2] = 1.0f;
	lightCB.pointLight[1].Color[3] = 1.0f;
	lightCB.pointLight[1].Intensity = 5000.0f;
	lightCB.pointLight[1].Position[0] = 800.0f;
	lightCB.pointLight[1].Position[1] = 0.0f;
	lightCB.pointLight[1].Position[2] = 800.0f;
	lightCB.pointLight[1].Range = 50000.0f;

	lightCB.pointLight[2].Color[0] = 1.0f;
	lightCB.pointLight[2].Color[1] = 1.0f;
	lightCB.pointLight[2].Color[2] = 1.0f;
	lightCB.pointLight[2].Color[3] = 1.0f;
	lightCB.pointLight[2].Intensity = 5000.0f;
	lightCB.pointLight[2].Position[0] = 800.0f;
	lightCB.pointLight[2].Position[1] = 0.0f;
	lightCB.pointLight[2].Position[2] = -800.0f;
	lightCB.pointLight[2].Range = 50000.0f;

	lightCB.dirLightCount = 3;
	lightCB.pointLightCount = 0;// 4 * lightCount * lightCount;

	auto LightCB = mCurrFrameResource->LightCB.get();
	LightCB->CopyData(0, lightCB);
}

void GDxRenderer::UpdateMaterialBuffer(const GGiGameTimer* gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for (auto& e : pMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		GRiMaterial* mat = e.second;
		if (mat->NumFramesDirty > 0)
		{
			MaterialData matData;
			int i;
			XMMATRIX matTransform = DirectX::XMMatrixScaling(mat->GetScaleX(), mat->GetScaleY(), 1.0f);
			//GGiFloat4x4* ggiMat = mat->MatTransform.get();
			//GDxFloat4x4* dxMat = dynamic_cast<GDxFloat4x4*>(ggiMat);
			//if (dxMat == nullptr)
				//ThrowDxException(L"Dynamic cast from GRiFloat4x4 to GDxFloat4x4 failed.");
			//XMMATRIX matTransform = XMLoadFloat4x4(&dxMat->GetValue());
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));

			size_t texNum = mat->GetTextureNum();
			if (texNum > MATERIAL_MAX_TEXTURE_NUM)
				ThrowDxException(L"Material (CBIndex : " + std::to_wstring(mat->MatIndex) + L" ) texture number exceeds MATERIAL_MAX_TEXTURE_NUM.");
			for (i = 0; i < texNum; i++)
			{
				auto texName = mat->GetTextureUniqueNameByIndex(i);
				if (pTextures.find(texName) == pTextures.end())
					ThrowGGiException(L"Texture" + texName + L" not found.");
				matData.TextureIndex[i] = pTextures[texName]->texIndex;
			}

			size_t scalarNum = mat->GetScalarNum();
			if (scalarNum > MATERIAL_MAX_SCALAR_NUM)
				ThrowDxException(L"Material (CBIndex : " + std::to_wstring(mat->MatIndex) + L" ) scalar number exceeds MATERIAL_MAX_SCALAR_NUM.");
			for (i = 0; i < scalarNum; i++)
			{
				matData.ScalarParams[i] = mat->GetScalar(i);
			}

			size_t vectorNum = mat->GetVectorNum();
			if (vectorNum > MATERIAL_MAX_VECTOR_NUM)
				ThrowDxException(L"Material (CBIndex : " + std::to_wstring(mat->MatIndex) + L" ) vector number exceeds MATERIAL_MAX_VECTOR_NUM.");
			for (i = 0; i < vectorNum; i++)
			{
				XMVECTOR ggiVec = mat->GetVector(i);
				DirectX::XMStoreFloat4(&matData.VectorParams[i], ggiVec);
			}
			//matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
			//matData.NormalMapIndex = mat->NormalSrvHeapIndex;
			//matData.Roughness = mat->Roughness;
			//matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			//matData.FresnelR0 = mat->FresnelR0;

			currMaterialBuffer->CopyData(mat->MatIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void GDxRenderer::UpdateSdfDescriptorBuffer(const GGiGameTimer* gt)
{
	auto currDescBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer.get();

	int soSdfIndex = 0;
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		if (so->GetMesh()->GetSdf() != nullptr && so->GetMesh()->GetSdf()->size() > 0)
		{
			so->UpdateTransform();
			mSceneObjectSdfDescriptors[soSdfIndex].SdfIndex = so->GetMesh()->mSdfIndex;
			auto trans = GDx::GGiToDxMatrix(so->GetTransform());
			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].objWorld, XMMatrixTranspose(trans));
			auto invTrans = DirectX::XMMatrixInverse(&XMMatrixDeterminant(trans), trans);
			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].objInvWorld, XMMatrixTranspose(invTrans));
			auto invTrans_IT = XMMatrixTranspose(trans);
			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].objInvWorld_IT, XMMatrixTranspose(invTrans_IT));

			currDescBuffer->CopyData(soSdfIndex, mSceneObjectSdfDescriptors[soSdfIndex]);

			soSdfIndex++;
		}
	}
	mSceneObjectSdfNum = soSdfIndex;
}

void GDxRenderer::UpdateShadowTransform(const GGiGameTimer* gt)
{
	// Only the first "main" light casts a shadow.
	XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
	XMVECTOR lightPos = -2.0f*mSceneBounds.Radius*lightDir;
	XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

	XMStoreFloat3(&mLightPosW, lightPos);

	// Transform bounding sphere to light space.
	XMFLOAT3 sphereCenterLS;
	XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

	// Ortho frustum in light space encloses scene.
	float l = sphereCenterLS.x - mSceneBounds.Radius;
	float b = sphereCenterLS.y - mSceneBounds.Radius;
	float n = sphereCenterLS.z - mSceneBounds.Radius;
	float r = sphereCenterLS.x + mSceneBounds.Radius;
	float t = sphereCenterLS.y + mSceneBounds.Radius;
	float f = sphereCenterLS.z + mSceneBounds.Radius;

	mLightNearZ = n;
	mLightFarZ = f;
	XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMMATRIX S = lightView * lightProj*T;
	XMStoreFloat4x4(&mLightView, lightView);
	XMStoreFloat4x4(&mLightProj, lightProj);
	XMStoreFloat4x4(&mShadowTransform, S);
}

void GDxRenderer::UpdateMainPassCB(const GGiGameTimer* gt)
{
	/*
	auto viewMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetView());
	if (viewMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto projMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetProj());
	if (projMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto prevViewProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetPrevViewProj());
	if (prevViewProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");
	*/

	UINT subsampIndex = mFrameCount % TAA_SAMPLE_COUNT;
	double JitterX = Halton_2[subsampIndex] / (double)mClientWidth * (double)TAA_JITTER_DISTANCE;
	double JitterY = Halton_3[subsampIndex] / (double)mClientHeight * (double)TAA_JITTER_DISTANCE;
	//XMMATRIX view = DirectX::XMLoadFloat4x4(&(viewMat->GetValue()));
	XMMATRIX view = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX proj = GDx::GGiToDxMatrix(pCamera->GetProj());
	proj.r[2].m128_f32[0] += (float)JitterX;//_31
	proj.r[2].m128_f32[1] += (float)JitterY;//_32
	//XMMATRIX proj = DirectX::XMLoadFloat4x4(&(projMat->GetValue()));
	//proj.r[2].m128_f32[0] += JitterX;
	//proj.r[2].m128_f32[1] += JitterY;

	XMMATRIX unjitteredProj = GDx::GGiToDxMatrix(pCamera->GetProj());
	XMMATRIX prevViewProj = GDx::GGiToDxMatrix(pCamera->GetPrevViewProj());

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX unjitteredViewProj = XMMatrixMultiply(view, unjitteredProj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f
	);

	XMMATRIX viewProjTex = XMMatrixMultiply(viewProj, T);
	XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.UnjitteredViewProj, XMMatrixTranspose(unjitteredViewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.PrevViewProj, XMMatrixTranspose(prevViewProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProjTex, XMMatrixTranspose(viewProjTex));
	XMStoreFloat4x4(&mMainPassCB.ShadowTransform, XMMatrixTranspose(shadowTransform));
	auto eyePos = pCamera->GetPosition();
	mMainPassCB.EyePosW = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt->TotalTime();
	mMainPassCB.DeltaTime = gt->DeltaTime();
	mMainPassCB.FrameCount = mFrameCount;
	mMainPassCB.Jitter = XMFLOAT2((float)(JitterX / 2), (float)(-JitterY / 2));//negate Y because world coord and tex coord have different Y axis.
	mMainPassCB.AmbientLight = { 0.4f, 0.4f, 0.6f, 1.0f };
	mMainPassCB.MainDirectionalLightDir = { 0.57735f, -0.57735f, -0.57735f, 0.0f };

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void GDxRenderer::UpdateSkyPassCB(const GGiGameTimer* gt)
{
	/*
	auto viewMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetView());
	if (viewMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto projMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetProj());
	if (projMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto prevViewProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetPrevViewProj());
	if (prevViewProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");
	*/

	XMMATRIX view = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX proj = GDx::GGiToDxMatrix(pCamera->GetProj());
	UINT subsampIndex = mFrameCount % TAA_SAMPLE_COUNT;
	double JitterX = Halton_2[subsampIndex] / (double)mClientWidth * (double)TAA_JITTER_DISTANCE;
	double JitterY = Halton_3[subsampIndex] / (double)mClientHeight * (double)TAA_JITTER_DISTANCE;
	proj.r[2].m128_f32[0] += (float)JitterX;
	proj.r[2].m128_f32[1] += (float)JitterY;

	XMMATRIX unjitteredProj = GDx::GGiToDxMatrix(pCamera->GetProj());
	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX unjitteredViewProj = XMMatrixMultiply(view, unjitteredProj);
	XMMATRIX prevViewProj = GDx::GGiToDxMatrix(pCamera->GetPrevViewProj());

	XMStoreFloat4x4(&mSkyPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mSkyPassCB.UnjitteredViewProj, XMMatrixTranspose(unjitteredViewProj));
	XMStoreFloat4x4(&mSkyPassCB.PrevViewProj, XMMatrixTranspose(prevViewProj));
	auto eyePos = pCamera->GetPosition();
	mSkyPassCB.EyePosW = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
	auto prevPos = pCamera->GetPrevPosition();
	mSkyPassCB.PrevPos = DirectX::XMFLOAT3(prevPos[0], prevPos[1], prevPos[2]);
	mSkyPassCB.roughness = 0.3f; // doesn't matter

	auto currPassCB = mCurrFrameResource->SkyCB.get();
	currPassCB->CopyData(0, mSkyPassCB);
}

void GDxRenderer::CullSceneObjects(const GGiGameTimer* gt)
{
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Reset cull state.
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		so->SetCullState(CullState::Visible);
	}

	numVisible = 0;
	numFrustumCulled = 0;
	numOcclusionCulled = 0;

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Frustum culling.
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	GGiCpuProfiler::GetInstance().StartCpuProfile("Frustum Culling");

	/*
	auto viewMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetView());
	if (viewMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto projMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetProj());
	if (projMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto revProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetReversedProj());
	if (revProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto prevViewProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetPrevViewProj());
	if (prevViewProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");
	*/

	XMMATRIX view = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX proj = GDx::GGiToDxMatrix(pCamera->GetProj());
	XMMATRIX revProj = GDx::GGiToDxMatrix(pCamera->GetReversedProj());
	XMMATRIX prevViewProj = GDx::GGiToDxMatrix(pCamera->GetPrevViewProj());
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	//proj.r[0].m128_f32[0] = (float)((double)proj.r[0].m128_f32[0] * ((double)mClientWidth / (double)mClientHeight) / ((double)DEPTH_READBACK_BUFFER_SIZE_X / (double)DEPTH_READBACK_BUFFER_SIZE_Y));

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invPrevViewProj = XMMatrixInverse(&XMMatrixDeterminant(prevViewProj), prevViewProj);

	BoundingFrustum cameraFrustum;
#if USE_REVERSE_Z
	BoundingFrustum::CreateFromMatrix(cameraFrustum, revProj);
#else
	BoundingFrustum::CreateFromMatrix(mCameraFrustum, proj);
#endif

	UINT32 fcStep;
	if (pSceneObjectLayer[(int)RenderLayer::Deferred].size() > 100)
		fcStep = (UINT32)(pSceneObjectLayer[(int)RenderLayer::Deferred].size() / mRendererThreadPool->GetThreadNum()) + 1;
	else
		fcStep = 100;
	for (auto i = 0u; i < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); i += fcStep)
	{
		mRendererThreadPool->Enqueue([&, i]//pSceneObjectLayer, &viewProj]
		{
			for (auto j = i; j < i + fcStep && j < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); j++)
			{
				auto so = pSceneObjectLayer[(int)RenderLayer::Deferred][j];

				XMMATRIX world = GDx::GGiToDxMatrix(so->GetTransform());

				XMMATRIX localToView = XMMatrixMultiply(world, view);

				BoundingBox bounds;
				bounds.Center = DirectX::XMFLOAT3(so->GetMesh()->bounds.Center);
				bounds.Extents = DirectX::XMFLOAT3(so->GetMesh()->bounds.Extents);

				BoundingBox worldBounds;
				bounds.Transform(worldBounds, localToView);

				// Perform the box/frustum intersection test in local space.
				if ((cameraFrustum.Contains(worldBounds) == DirectX::DISJOINT))
				{
					so->SetCullState(CullState::FrustumCulled);
				}
			}
		}
		);
	}

	mRendererThreadPool->Flush();

	GGiCpuProfiler::GetInstance().EndCpuProfile("Frustum Culling");

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Occlusion culling
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	if (mFrameCount != 0)
	{

		GGiCpuProfiler::GetInstance().StartCpuProfile("Occlusion Culling");

		// Map the data so we can read it on CPU.
		D3D12_RANGE readbackBufferRange = { 0, 4 * DEPTH_READBACK_BUFFER_SIZE };
		float* depthReadbackBuffer = nullptr;
		static float outputTest[DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y];
		static float reprojectedDepthBuffer[DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y];
		ThrowIfFailed(mDepthReadbackBuffer->Map(0, &readbackBufferRange, reinterpret_cast<void**>(&depthReadbackBuffer)));

#if 0
		std::ofstream fout;
#if 0
		for (auto i = 0u; i < DEPTH_READBACK_BUFFER_SIZE; i++)
			depthReadbackBuffer[i] *= 10;
#endif
		fout.open("depth.raw", ios::out | ios::binary);
		fout.write(reinterpret_cast<char*>(depthReadbackBuffer), DEPTH_READBACK_BUFFER_SIZE * 4);
		fout.close();
#if 0
		for (auto i = 0u; i < DEPTH_READBACK_BUFFER_SIZE; i++)
			depthReadbackBuffer[i] /= 10;
#endif
#endif

		D3D12_RANGE emptyRange = { 0, 0 };
		mDepthReadbackBuffer->Unmap(0, &emptyRange);

		// Reproject depth buffer.
		//GGiCpuProfiler::GetInstance().StartCpuProfile("Reprojection");

#if USE_MASKED_DEPTH_BUFFER
		GRiOcclusionCullingRasterizer::GetInstance().ReprojectToMaskedBufferMT(
			mRendererThreadPool.get(),
			depthReadbackBuffer,
			viewProj.r,
			invPrevViewProj.r
		);
#else
		GRiOcclusionCullingRasterizer::GetInstance().Reproject(
			depthReadbackBuffer,
			reprojectedDepthBuffer,
			viewProj.r,
			invPrevViewProj.r
		);
#endif

#if 0
		GRiOcclusionCullingRasterizer::GetInstance().GenerateMaskedBufferDebugImage(outputTest);
#endif

		//GGiCpuProfiler::GetInstance().EndCpuProfile("Reprojection");

		//XMMATRIX worldViewProj;

		//GGiCpuProfiler::GetInstance().StartCpuProfile("Rasterization");

		UINT32 ocStep;
		if (pSceneObjectLayer[(int)RenderLayer::Deferred].size() > 100)
			ocStep = (UINT32)(pSceneObjectLayer[(int)RenderLayer::Deferred].size() / mRendererThreadPool->GetThreadNum()) + 1;
		else
			ocStep = 100;
		for (auto i = 0u; i < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); i += ocStep)
		{
			mRendererThreadPool->Enqueue([&, i]//pSceneObjectLayer, &viewProj]
			{
				for (auto j = i; j < i + ocStep && j < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); j++)
				{
					auto so = pSceneObjectLayer[(int)RenderLayer::Deferred][j];

					if (so->GetCullState() == CullState::FrustumCulled)
						continue;

					XMMATRIX sceneObjectTrans = GDx::GGiToDxMatrix(so->GetTransform());

					XMMATRIX worldViewProj = XMMatrixMultiply(sceneObjectTrans, viewProj);

#if USE_MASKED_DEPTH_BUFFER
					auto bOccCulled = !GRiOcclusionCullingRasterizer::GetInstance().RectTestBBoxMasked(
						so->GetMesh()->bounds,
						worldViewProj.r
					);
#else
					auto bOccCulled = !GRiOcclusionCullingRasterizer::GetInstance().RasterizeAndTestBBox(
						so->GetMesh()->bounds,
						worldViewProj.r,
						reprojectedDepthBuffer,
						outputTest
					);
#endif

					if (bOccCulled)
					{
						so->SetCullState(CullState::OcclusionCulled);
					}

				}
			}
			);
		}

		mRendererThreadPool->Flush();

		//GGiCpuProfiler::GetInstance().EndCpuProfile("Rasterization");

		for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
		{
			if (so->GetCullState() == CullState::OcclusionCulled)
			{
				numOcclusionCulled++;
			}
			else if (so->GetCullState() == CullState::FrustumCulled)
			{
				numFrustumCulled++;
			}
			else
			{
				numVisible++;
			}
		}

#if 0
		std::ofstream testOut;
		testOut.open("output.raw", ios::out | ios::binary);
#if 0
		for (auto i = 0u; i < DEPTH_READBACK_BUFFER_SIZE; i++)
			outputTest[i] *= 10;
#endif
		testOut.write(reinterpret_cast<char*>(outputTest), DEPTH_READBACK_BUFFER_SIZE * 4);
		testOut.close();
#endif

		GGiCpuProfiler::GetInstance().EndCpuProfile("Occlusion Culling");
	}
}

#pragma endregion

#pragma region Initialization

void GDxRenderer::SetImgui(GRiImgui* imguiPtr)
{
	GDxImgui* dxImgui = dynamic_cast<GDxImgui*>(imguiPtr);
	if (dxImgui == nullptr)
		ThrowGGiException("Cast failed from GRiImgui* to GDxImgui*.");
	pImgui = dxImgui;
}

void GDxRenderer::BuildRootSignature()
{
	// GBuffer root signature
	{
		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_TEXTURE_NUM, 0);

		CD3DX12_ROOT_PARAMETER gBufferRootParameters[5];
		gBufferRootParameters[0].InitAsConstantBufferView(0);
		gBufferRootParameters[1].InitAsConstants(1, 0, 1);
		gBufferRootParameters[2].InitAsConstantBufferView(1);
		gBufferRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gBufferRootParameters[4].InitAsShaderResourceView(0, 1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gBufferRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["GBuffer"].GetAddressOf())));
	}

	// Depth downsample pass root signature
	{
		//depth inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)1, 0);

		CD3DX12_ROOT_PARAMETER depthDownsampleRootParameters[3];
		depthDownsampleRootParameters[0].InitAsConstantBufferView(1);
		depthDownsampleRootParameters[1].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		depthDownsampleRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, depthDownsampleRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["DepthDownsamplePass"].GetAddressOf())));
	}

	// GBufferDebug root signature
	{
		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, 0);

		CD3DX12_ROOT_PARAMETER gBufferDebugRootParameters[5];
		gBufferDebugRootParameters[0].InitAsConstantBufferView(0);
		gBufferDebugRootParameters[1].InitAsConstants(1, 0, 1);
		gBufferDebugRootParameters[2].InitAsConstantBufferView(1);
		gBufferDebugRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gBufferDebugRootParameters[4].InitAsShaderResourceView(0, 1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gBufferDebugRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["GBufferDebug"].GetAddressOf())));
	}

	// Tile/cluster pass signature
	{

		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		CD3DX12_ROOT_PARAMETER gLightPassRootParameters[4];
		gLightPassRootParameters[0].InitAsConstantBufferView(0);
		gLightPassRootParameters[1].InitAsConstantBufferView(1);
		gLightPassRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[3].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, gLightPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["TileClusterPass"].GetAddressOf())));
	}

	// Screen space shadow pass signature
	{
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_SCENE_OBJECT_NUM, 0, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 2);

		CD3DX12_ROOT_PARAMETER gScreenSpaceShadowRootParameters[6];
		gScreenSpaceShadowRootParameters[0].InitAsConstants(1, 0);
		gScreenSpaceShadowRootParameters[1].InitAsShaderResourceView(0, 0);
		gScreenSpaceShadowRootParameters[2].InitAsShaderResourceView(1, 0);
		gScreenSpaceShadowRootParameters[3].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		gScreenSpaceShadowRootParameters[4].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gScreenSpaceShadowRootParameters[5].InitAsConstantBufferView(1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, gScreenSpaceShadowRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["ScreenSpaceShadowPass"].GetAddressOf())));
	}

	// Light pass signature
	{
		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, 1);

		//Depth inputs
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 1);

		//Shadow inputs
		CD3DX12_DESCRIPTOR_RANGE rangeShadow;
		rangeShadow.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 2);

		//IBL inputs
		CD3DX12_DESCRIPTOR_RANGE rangeIBL;
		rangeIBL.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)mPrefilterLevels + (UINT)1 + (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 3);

		CD3DX12_ROOT_PARAMETER gLightPassRootParameters[7];
		gLightPassRootParameters[0].InitAsConstantBufferView(0);
		gLightPassRootParameters[1].InitAsConstantBufferView(1);
		gLightPassRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[4].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[5].InitAsDescriptorTable(1, &rangeShadow, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[6].InitAsDescriptorTable(1, &rangeIBL, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(7, gLightPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;
		
		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["LightPass"].GetAddressOf())));
	}

	// Taa pass signature
	{
		//TAA inputs
		CD3DX12_DESCRIPTOR_RANGE rangeLight;
		rangeLight.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE rangeHistory;
		rangeHistory.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_ROOT_PARAMETER gTaaPassRootParameters[5];
		gTaaPassRootParameters[0].InitAsConstantBufferView(1);
		gTaaPassRootParameters[1].InitAsDescriptorTable(1, &rangeLight, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[2].InitAsDescriptorTable(1, &rangeHistory, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[3].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[4].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gTaaPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP
			);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["TaaPass"].GetAddressOf())));
	}

	// Motion blur pass signature
	{
		//Motion blur inputs
		CD3DX12_DESCRIPTOR_RANGE rangeLight;
		rangeLight.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_ROOT_PARAMETER gTaaPassRootParameters[3];
		gTaaPassRootParameters[0].InitAsConstantBufferView(1);
		gTaaPassRootParameters[1].InitAsDescriptorTable(1, &rangeLight, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[2].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, gTaaPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP
		);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["MotionBlurPass"].GetAddressOf())));
	}

	// Post process signature
	{
		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE ppInputRange;
		ppInputRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		//CD3DX12_DESCRIPTOR_RANGE skyRange;
		//skyRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mRtvHeaps["LightPass"]->mRtvHeap.HeapDesc.NumDescriptors, 1);

		CD3DX12_ROOT_PARAMETER gPostProcessRootParameters[1];
		gPostProcessRootParameters[0].InitAsDescriptorTable(1, &ppInputRange, D3D12_SHADER_VISIBILITY_ALL);
		//gPostProcessRootParameters[1].InitAsDescriptorTable(1, &skyRange, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, gPostProcessRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["PostProcess"].GetAddressOf())));
	}

	// SDF debug signature
	{
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_SCENE_OBJECT_NUM, 0, 1);

		CD3DX12_ROOT_PARAMETER gSdfDebugRootParameters[5];
		gSdfDebugRootParameters[0].InitAsConstants(1, 0);
		gSdfDebugRootParameters[1].InitAsShaderResourceView(0, 0);
		gSdfDebugRootParameters[2].InitAsShaderResourceView(1, 0);
		gSdfDebugRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gSdfDebugRootParameters[4].InitAsConstantBufferView(1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gSdfDebugRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SdfDebug"].GetAddressOf())));
	}

	// Sky root signature
	{
		CD3DX12_DESCRIPTOR_RANGE texTable0;
		texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);

		// Root parameter can be a table, root descriptor or root constants.
		CD3DX12_ROOT_PARAMETER slotRootParameter[3];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsConstantBufferView(1);
		slotRootParameter[2].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		//ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Sky"].GetAddressOf())));
	}

	// Forward root signature
	{
		CD3DX12_DESCRIPTOR_RANGE texTable0;
		texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0);

		CD3DX12_DESCRIPTOR_RANGE texTable1;
		texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)pTextures.size(), 3, 0);//10,3,0

		// Root parameter can be a table, root descriptor or root constants.
		CD3DX12_ROOT_PARAMETER slotRootParameter[5];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsConstantBufferView(1);
		slotRootParameter[2].InitAsShaderResourceView(0, 1);
		slotRootParameter[3].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[4].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

		auto staticSamplers = GetStaticSamplers();

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		//ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Forward"].GetAddressOf())));
	}
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GDxRenderer::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow
	};
}

void GDxRenderer::BuildDescriptorHeaps()
{
	//
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = MAX_TEXTURE_NUM
		+ MAX_SCENE_OBJECT_NUM //sdf textures
		+ 1 //imgui
		+ 1 //sky cubemap
		+ 1 //depth buffer
		+ 2 //downsampled depth buffer
		+ 1 //stencil buffer
		+ 4 //g-buffer
		+ 2 //tile/cluster pass srv and uav
		+ 1 //screen space shadow
		+ 1 //light pass
		+ 3 //taa
		+ 1 //motion blur
		+ (2 + mPrefilterLevels);//IBL
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));
	
	//
	// Fill out the heap with actual descriptors.
	//

	mSkyTexHeapIndex = 1; // 0 is for imgui.

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	//auto skyCubeMap = mTextures["skyCubeMap"]->Resource;
	D3D12_SHADER_RESOURCE_VIEW_DESC skySrvDesc = {};
	skySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	skySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	skySrvDesc.TextureCube.MostDetailedMip = 0;
	GDxTexture* tex = dynamic_cast<GDxTexture*>(pTextures[L"skyCubeMap"]);
	if (tex == nullptr)
		ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");
	skySrvDesc.TextureCube.MipLevels = tex->Resource->GetDesc().MipLevels;
	skySrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	skySrvDesc.Format = tex->Resource->GetDesc().Format;
	md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &skySrvDesc, GetCpuSrv(mSkyTexHeapIndex));

	// Build SRV for depth/stencil buffer
	{
		mDepthBufferSrvIndex = mSkyTexHeapIndex + 1;
		//mStencilBufferSrvIndex = mDepthBufferSrvIndex + 1;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; //DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &srvDesc, GetCpuSrv(mDepthBufferSrvIndex));

		D3D12_SHADER_RESOURCE_VIEW_DESC stencilSrvDesc = {};
		stencilSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		//stencilSrvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1,1,1,1);
		stencilSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		stencilSrvDesc.Texture2D.MipLevels = 1;
		stencilSrvDesc.Texture2D.MostDetailedMip = 0;
		stencilSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		//stencilSrvDesc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
		stencilSrvDesc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

		//md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &stencilSrvDesc, GetCpuSrv(mStencilBufferSrvIndex));
	}

	// Build SRV for depth readback buffer.
	{
		mDepthDownsampleSrvIndex = mDepthBufferSrvIndex + 1;

		GDxUavProperties prop;
		prop.mUavFormat = DXGI_FORMAT_R32_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		UINT64 elementNum = DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y;

		auto depthDownsamplePassUav = std::make_unique<GDxUav>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mDepthDownsampleSrvIndex), GetGpuSrv(mDepthDownsampleSrvIndex), prop, false, false, false, sizeof(float), elementNum);
		mUavs["DepthDownsamplePass"] = std::move(depthDownsamplePassUav);
	}

	// Build RTV heap and SRV for GBuffers.
	{
		mGBufferSrvIndex = mDepthDownsampleSrvIndex + mUavs["DepthDownsamplePass"]->GetSize();
		mVelocityBufferSrvIndex = mGBufferSrvIndex + 2;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R8G8B8A8_UNORM,//Albedo
			DXGI_FORMAT_R8G8B8A8_SNORM, //Normal
			//DXGI_FORMAT_R32G32B32A32_FLOAT, //WorldPos
			DXGI_FORMAT_R16G16_FLOAT, //Velocity
			DXGI_FORMAT_R8G8B8A8_UNORM //OcclusionRoughnessMetallic
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,0 },
			{ 0,0,0,0 },
			//{ 0,0,0,0 },
			{ 0,0,0,0 },
			{ 0,0.3f,0,0 }
		};
		std::vector<GRtvProperties> propVec;
		for (size_t i = 0; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto gBufferRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mGBufferSrvIndex), GetGpuSrv(mGBufferSrvIndex), propVec);
		mRtvHeaps["GBuffer"] = std::move(gBufferRtvHeap);
	}

	// Build UAV and SRV for Tile/Cluster pass.
	{
		mTileClusterSrvIndex = mGBufferSrvIndex + mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors;

		GDxUavProperties prop;
		prop.mUavFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		UINT64 elementNum = 0;
#if USE_TBDR
		elementNum = UINT64(ceilf((float)mClientWidth / (float)TILE_SIZE_X) * ceilf((float)mClientHeight / (float)TILE_SIZE_Y) + 0.01);
#elif USE_CBDR
		elementNum = (UINT64)(ceilf((float)mClientWidth / (float)CLUSTER_SIZE_X) * ceilf((float)mClientHeight / (float)CLUSTER_SIZE_Y) * CLUSTER_NUM_Z + 0.01);
#else
		ThrowGGiException("TBDR/CBDR not enabled");
#endif

		auto tileClusterPassUav = std::make_unique<GDxUav>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mTileClusterSrvIndex), GetGpuSrv(mTileClusterSrvIndex), prop, false, false, false, sizeof(LightList), elementNum);
		mUavs["TileClusterPass"] = std::move(tileClusterPassUav);
	}

	// Build RTV and SRV for screen space shadow pass.
	{
		mScreenSpaceShadowPassSrvIndex = mTileClusterSrvIndex + mUavs["TileClusterPass"]->GetSize();

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R32_FLOAT// Direct light and ambient light
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 1,1,1,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto screenSpaceShadowPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mScreenSpaceShadowPassSrvIndex), GetGpuSrv(mScreenSpaceShadowPassSrvIndex), propVec);
		mRtvHeaps["ScreenSpaceShadowPass"] = std::move(screenSpaceShadowPassRtvHeap);
	}

	// Build RTV and SRV for light pass.
	{
		mLightPassSrvIndex = mScreenSpaceShadowPassSrvIndex + (UINT)mRtvHeaps["ScreenSpaceShadowPass"]->mRtv.size();

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R32G32B32A32_FLOAT// Direct light and ambient light
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto lightPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mLightPassSrvIndex), GetGpuSrv(mLightPassSrvIndex), propVec);
		mRtvHeaps["LightPass"] = std::move(lightPassRtvHeap);
	}

	// Build RTV heap and SRV for TAA pass.
	{
		mTaaPassSrvIndex = mLightPassSrvIndex + (UINT)mRtvHeaps["LightPass"]->mRtv.size();

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R32G32B32A32_FLOAT,// TAA History 1
			DXGI_FORMAT_R32G32B32A32_FLOAT,// TAA History 2
			DXGI_FORMAT_R32G32B32A32_FLOAT// TAA Output
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,1 },
			{ 0,0,0,1 },
			{ 0,0,0,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto taaPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mTaaPassSrvIndex), GetGpuSrv(mTaaPassSrvIndex), propVec);
		mRtvHeaps["TaaPass"] = std::move(taaPassRtvHeap);
	}

	// Build RTV heap and SRV for motion blur pass.
	{
		mMotionBlurSrvIndex = mTaaPassSrvIndex + mRtvHeaps["TaaPass"]->mRtvHeap.HeapDesc.NumDescriptors;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R32G32B32A32_FLOAT// Motion Blur Output
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto motionBlurPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mMotionBlurSrvIndex), GetGpuSrv(mMotionBlurSrvIndex), propVec);
		mRtvHeaps["MotionBlurPass"] = std::move(motionBlurPassRtvHeap);
	}

	// Build cubemap SRV and RTVs for irradiance pre-integration.
	{
		mIblIndex = mMotionBlurSrvIndex + mRtvHeaps["TaaPass"]->mRtvHeap.HeapDesc.NumDescriptors;

		GRtvProperties prop;
		//prop.mRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prop.mRtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		auto gIrradianceCubemap = std::make_unique<GDxCubeRtv>(md3dDevice.Get(), SKY_CUBEMAP_SIZE, GetCpuSrv(mIblIndex), GetGpuSrv(mIblIndex), prop);
		mCubeRtvs["Irradiance"] = std::move(gIrradianceCubemap);
	}

	// Build SRV for LUT
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		GDxTexture* tex = dynamic_cast<GDxTexture*>(pTextures[L"Resource\\Textures\\IBL_BRDF_LUT.png"]);
		if (tex == nullptr)
			ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");

		srvDesc.Format = tex->Resource->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, GetCpuSrv(mIblIndex + 1));
	}

	// Build cubemap SRV and RTVs for prefilter cubemap pre-integration.
	{
		for (auto i = 0u; i < mPrefilterLevels; i++)
		{
			GRtvProperties prop;
			//prop.mRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
			prop.mRtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			prop.mClearColor[0] = 0;
			prop.mClearColor[1] = 0;
			prop.mClearColor[2] = 0;
			prop.mClearColor[3] = 1;

			auto gPrefilterCubemap = std::make_unique<GDxCubeRtv>(md3dDevice.Get(), (UINT)(SKY_CUBEMAP_SIZE / pow(2, i)), GetCpuSrv(mIblIndex + 2 + i), GetGpuSrv(mIblIndex + 2 + i), prop);
			mCubeRtvs["Prefilter_" + std::to_string(i)] = std::move(gPrefilterCubemap);
		}
	}

	// Build SRV for ordinary textures.
	{
		mTextrueHeapIndex = mIblIndex + 2 + mPrefilterLevels;

		for (auto i = 0u; i < MAX_TEXTURE_NUM; i++)
			mTexturePoolFreeIndex.push_back(i);

		for (auto tex : pTextures)
		{
			RegisterTexture(tex.second);
		}
	}

	// Build SRV for SDF textures.
	{
		mSdfTextrueIndex = mTextrueHeapIndex + MAX_TEXTURE_NUM;
	}
}

void GDxRenderer::BuildPSOs()
{
	// PSO for GBuffers.
	{
		D3D12_DEPTH_STENCIL_DESC gBufferDSD;
		gBufferDSD.DepthEnable = true;
		gBufferDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		gBufferDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		gBufferDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		gBufferDSD.StencilEnable = true;
		gBufferDSD.StencilReadMask = 0xff;
		gBufferDSD.StencilWriteMask = 0xff;
		gBufferDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gBufferDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		gBufferDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gBufferDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC gBufferPsoDesc;
		ZeroMemory(&gBufferPsoDesc, sizeof(gBufferPsoDesc));
		gBufferPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\DefaultVS.cso");
		gBufferPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\DeferredPS.cso");
		gBufferPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		gBufferPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		gBufferPsoDesc.pRootSignature = mRootSignatures["GBuffer"].Get();
		//gBufferPsoDesc.pRootSignature = mRootSignatures["Forward"].Get();
		gBufferPsoDesc.DepthStencilState = gBufferDSD;
		gBufferPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		gBufferPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		gBufferPsoDesc.SampleMask = UINT_MAX;
		gBufferPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		gBufferPsoDesc.NumRenderTargets = (UINT)mRtvHeaps["GBuffer"]->mRtv.size();
		for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		{
			gBufferPsoDesc.RTVFormats[i] = mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mRtvFormat;
		}
		gBufferPsoDesc.DSVFormat = mDepthStencilFormat;
		gBufferPsoDesc.SampleDesc.Count = 1;// don't use msaa in deferred rendering.
		//deferredPSO = sysRM->CreatePSO(StringID("deferredPSO"), descPipelineState);
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gBufferPsoDesc, IID_PPV_ARGS(&mPSOs["GBuffer"])));
	}

	// PSO for depth downsample pass
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		computePsoDesc.pRootSignature = mRootSignatures["DepthDownsamplePass"].Get();
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\DepthDownsampleCS.cso");
		computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["DepthDownsamplePass"])));
	}

	// PSO for tile/cluster pass.
	{
		/*
		D3D12_DEPTH_STENCIL_DESC lightPassDSD;
		lightPassDSD.DepthEnable = false;
		lightPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		lightPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		lightPassDSD.StencilEnable = true;
		lightPassDSD.StencilReadMask = 0xff;
		lightPassDSD.StencilWriteMask = 0x0;
		lightPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		// We are not rendering backfacing polygons, so these settings do not matter.
		lightPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		auto blendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		blendState.AlphaToCoverageEnable = false;
		blendState.IndependentBlendEnable = false;

		blendState.RenderTarget[0].BlendEnable = true;
		blendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		blendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;

		auto rasterizer = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		//rasterizer.CullMode = D3D12_CULL_MODE_FRONT; // Front culling for point light
		rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		rasterizer.DepthClipEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPipelineState;
		ZeroMemory(&descPipelineState, sizeof(descPipelineState));

		descPipelineState.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPipelineState.PS = GDxShaderManager::LoadShader(L"Shaders\\DirectLightPassPS.cso");
		descPipelineState.pRootSignature = mRootSignatures["LightPass"].Get();
		descPipelineState.BlendState = blendState;
		descPipelineState.DepthStencilState = lightPassDSD;
		descPipelineState.DepthStencilState.DepthEnable = false;
		descPipelineState.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPipelineState.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPipelineState.RasterizerState = rasterizer;
		descPipelineState.NumRenderTargets = 1;
		descPipelineState.RTVFormats[0] = mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mRtvFormat;
		descPipelineState.SampleMask = UINT_MAX;
		descPipelineState.SampleDesc.Count = 1;
		descPipelineState.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPipelineState, IID_PPV_ARGS(&mPSOs["DirectLightPass"])));
		*/

		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		computePsoDesc.pRootSignature = mRootSignatures["TileClusterPass"].Get();
#if USE_TBDR
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\TiledDeferredCS.cso");
#elif USE_CBDR
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\ClusteredDeferredCS.cso");
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif
		computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["TileClusterPass"])));
	}

	// PSO for screen space shadow pass.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC ScreenSpaceShadowPsoDesc;

		D3D12_DEPTH_STENCIL_DESC ScreenSpaceShadowDSD;
		ScreenSpaceShadowDSD.DepthEnable = true;
		ScreenSpaceShadowDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
#if USE_REVERSE_Z
		ScreenSpaceShadowDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		ScreenSpaceShadowDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		ScreenSpaceShadowDSD.StencilEnable = false;

		ZeroMemory(&ScreenSpaceShadowPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		ScreenSpaceShadowPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		ScreenSpaceShadowPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		ScreenSpaceShadowPsoDesc.pRootSignature = mRootSignatures["ScreenSpaceShadowPass"].Get();
		ScreenSpaceShadowPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		ScreenSpaceShadowPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\ScreenSpaceShadowPS.cso");
		ScreenSpaceShadowPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		ScreenSpaceShadowPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		ScreenSpaceShadowPsoDesc.DepthStencilState = ScreenSpaceShadowDSD;
		ScreenSpaceShadowPsoDesc.SampleMask = UINT_MAX;
		ScreenSpaceShadowPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ScreenSpaceShadowPsoDesc.NumRenderTargets = 1;
		ScreenSpaceShadowPsoDesc.RTVFormats[0] = mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mRtvFormat;//light pass output
		ScreenSpaceShadowPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		ScreenSpaceShadowPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		ScreenSpaceShadowPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ScreenSpaceShadowPsoDesc, IID_PPV_ARGS(&mPSOs["ScreenSpaceShadowPass"])));
	}

	// PSO for light pass.
	{
		D3D12_DEPTH_STENCIL_DESC lightPassDSD;
		lightPassDSD.DepthEnable = true;
		lightPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		lightPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		lightPassDSD.StencilEnable = false;
		lightPassDSD.StencilReadMask = 0xff;
		lightPassDSD.StencilWriteMask = 0x0;
		lightPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		lightPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descLightPSO;
		ZeroMemory(&descLightPSO, sizeof(descLightPSO));

		descLightPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descLightPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\LightPassPS.cso");
		descLightPSO.pRootSignature = mRootSignatures["LightPass"].Get();
		descLightPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descLightPSO.DepthStencilState = lightPassDSD;
		//descLightPSO.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		descLightPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descLightPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descLightPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descLightPSO.NumRenderTargets = 1;
		descLightPSO.RTVFormats[0] = mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mRtvFormat;//light pass output
		descLightPSO.SampleMask = UINT_MAX;
		descLightPSO.SampleDesc.Count = 1;
		descLightPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descLightPSO, IID_PPV_ARGS(&mPSOs["LightPass"])));
	}

	// PSO for TAA pass.
	{
		D3D12_DEPTH_STENCIL_DESC taaPassDSD;
		taaPassDSD.DepthEnable = true;
		taaPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		taaPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		taaPassDSD.StencilEnable = false;
		taaPassDSD.StencilReadMask = 0xff;
		taaPassDSD.StencilWriteMask = 0x0;
		taaPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		taaPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descTaaPSO;
		ZeroMemory(&descTaaPSO, sizeof(descTaaPSO));

		descTaaPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descTaaPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\TaaPassPS.cso"); 
		descTaaPSO.pRootSignature = mRootSignatures["TaaPass"].Get();
		descTaaPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descTaaPSO.DepthStencilState = taaPassDSD;
		//descTaaPSO.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		descTaaPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descTaaPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descTaaPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descTaaPSO.NumRenderTargets = 2;
		descTaaPSO.RTVFormats[0] = mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mRtvFormat;//taa output
		descTaaPSO.RTVFormats[1] = mRtvHeaps["TaaPass"]->mRtv[0]->mProperties.mRtvFormat;//history
		descTaaPSO.SampleMask = UINT_MAX;
		descTaaPSO.SampleDesc.Count = 1;
		descTaaPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descTaaPSO, IID_PPV_ARGS(&mPSOs["TaaPass"])));
	}

	// PSO for motion blur pass.
	{
		D3D12_DEPTH_STENCIL_DESC motionBlurPassDSD;
		motionBlurPassDSD.DepthEnable = true;
		motionBlurPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		motionBlurPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		motionBlurPassDSD.StencilEnable = false;
		motionBlurPassDSD.StencilReadMask = 0xff;
		motionBlurPassDSD.StencilWriteMask = 0x0;
		motionBlurPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		motionBlurPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descMotionBlurPSO;
		ZeroMemory(&descMotionBlurPSO, sizeof(descMotionBlurPSO));

		descMotionBlurPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descMotionBlurPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurPassPS.cso");
		descMotionBlurPSO.pRootSignature = mRootSignatures["MotionBlurPass"].Get();
		descMotionBlurPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descMotionBlurPSO.DepthStencilState = motionBlurPassDSD;
		//descMotionBlurPSO.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		descMotionBlurPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descMotionBlurPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descMotionBlurPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descMotionBlurPSO.NumRenderTargets = 1;
		descMotionBlurPSO.RTVFormats[0] = mRtvHeaps["MotionBlurPass"]->mRtv[0]->mProperties.mRtvFormat;//motion blur output
		descMotionBlurPSO.SampleMask = UINT_MAX;
		descMotionBlurPSO.SampleDesc.Count = 1;
		descMotionBlurPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descMotionBlurPSO, IID_PPV_ARGS(&mPSOs["MotionBlurPass"])));
	}

	// PSO for post process.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PostProcessPsoDesc;

		D3D12_DEPTH_STENCIL_DESC postProcessDSD;
		postProcessDSD.DepthEnable = true;
		postProcessDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
#if USE_REVERSE_Z
		postProcessDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		postProcessDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		postProcessDSD.StencilEnable = false;
		postProcessDSD.StencilReadMask = 0xff;
		postProcessDSD.StencilWriteMask = 0x0;
		postProcessDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		postProcessDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

		ZeroMemory(&PostProcessPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		PostProcessPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		PostProcessPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		PostProcessPsoDesc.pRootSignature = mRootSignatures["PostProcess"].Get();
		PostProcessPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		PostProcessPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\PostProcessPS.cso");
		PostProcessPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PostProcessPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PostProcessPsoDesc.DepthStencilState = postProcessDSD;
		PostProcessPsoDesc.SampleMask = UINT_MAX;
		PostProcessPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PostProcessPsoDesc.NumRenderTargets = 1;
		PostProcessPsoDesc.RTVFormats[0] = mBackBufferFormat;
		PostProcessPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		PostProcessPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		PostProcessPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&PostProcessPsoDesc, IID_PPV_ARGS(&mPSOs["PostProcess"])));
	}

	// PSO for SDF debug.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC SdfDebugPsoDesc;

		D3D12_DEPTH_STENCIL_DESC SdfDebugDSD;
		SdfDebugDSD.DepthEnable = true;
		SdfDebugDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
#if USE_REVERSE_Z
		SdfDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		SdfDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		SdfDebugDSD.StencilEnable = false;

		ZeroMemory(&SdfDebugPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		SdfDebugPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		SdfDebugPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		SdfDebugPsoDesc.pRootSignature = mRootSignatures["SdfDebug"].Get();
		SdfDebugPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		SdfDebugPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\SdfDebugPS.cso");
		SdfDebugPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		SdfDebugPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		SdfDebugPsoDesc.DepthStencilState = SdfDebugDSD;
		SdfDebugPsoDesc.SampleMask = UINT_MAX;
		SdfDebugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		SdfDebugPsoDesc.NumRenderTargets = 1;
		SdfDebugPsoDesc.RTVFormats[0] = mBackBufferFormat;
		SdfDebugPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		SdfDebugPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		SdfDebugPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&SdfDebugPsoDesc, IID_PPV_ARGS(&mPSOs["SdfDebug"])));
	}

	// PSO for GBuffer debug layer.
	{
		D3D12_DEPTH_STENCIL_DESC gBufferDebugDSD;
		gBufferDebugDSD.DepthEnable = true;
		gBufferDebugDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		gBufferDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		gBufferDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		gBufferDebugDSD.StencilEnable = false;
		gBufferDebugDSD.StencilReadMask = 0xff;
		gBufferDebugDSD.StencilWriteMask = 0x0;
		gBufferDebugDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		gBufferDebugDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc;

		ZeroMemory(&debugPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		debugPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		debugPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		//debugPsoDesc.pRootSignature = mRootSignatures["Forward"].Get();
		debugPsoDesc.pRootSignature = mRootSignatures["GBufferDebug"].Get();
		debugPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		debugPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		debugPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		debugPsoDesc.SampleMask = UINT_MAX;
		debugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		debugPsoDesc.NumRenderTargets = 1;
		debugPsoDesc.RTVFormats[0] = mBackBufferFormat;
		debugPsoDesc.SampleDesc.Count = 1;
		debugPsoDesc.SampleDesc.Quality = 0;
		debugPsoDesc.DSVFormat = mDepthStencilFormat;
		debugPsoDesc.pRootSignature = mRootSignatures["GBufferDebug"].Get();
		debugPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\ScreenVS.cso");
		debugPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\GBufferDebugPS.cso");
		debugPsoDesc.DepthStencilState = gBufferDebugDSD;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["GBufferDebug"])));
	}

	// PSO for sky.
	{
		D3D12_DEPTH_STENCIL_DESC gskyDSD;
		gskyDSD.DepthEnable = true;
		gskyDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		gskyDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		gskyDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		gskyDSD.StencilEnable = false;
		gskyDSD.StencilReadMask = 0xff;
		gskyDSD.StencilWriteMask = 0x0;
		gskyDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gskyDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		gskyDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gskyDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		auto skyBlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		skyBlendState.AlphaToCoverageEnable = false;
		skyBlendState.IndependentBlendEnable = false;
		skyBlendState.RenderTarget[0].BlendEnable = true;
		skyBlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		skyBlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		skyBlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc;

		ZeroMemory(&skyPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		skyPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		skyPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		skyPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		//skyPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		skyPsoDesc.BlendState = skyBlendState;
		//skyPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		//skyPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		skyPsoDesc.DepthStencilState = gskyDSD;
		skyPsoDesc.SampleMask = UINT_MAX;
		skyPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		skyPsoDesc.NumRenderTargets = 2;
		skyPsoDesc.RTVFormats[0] = mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mRtvFormat;
		skyPsoDesc.RTVFormats[1] = mRtvHeaps["GBuffer"]->mRtv[mVelocityBufferSrvIndex - mGBufferSrvIndex]->mProperties.mRtvFormat;
		skyPsoDesc.SampleDesc.Count = 1;
		skyPsoDesc.SampleDesc.Quality = 0;
		skyPsoDesc.DSVFormat = mDepthStencilFormat;

		// The camera is inside the sky sphere, so just turn off culling.
		//skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;

		// Make sure the depth function is LESS_EQUAL and not just LESS.  
		// Otherwise, the normalized depth values at z = 1 (NDC) will 
		// fail the depth test if the depth buffer was cleared to 1.
		skyPsoDesc.pRootSignature = mRootSignatures["Sky"].Get();
		skyPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\SkyVS.cso");
		skyPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\SkyPS.cso");
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["Sky"])));
	}

	// PSO for irradiance pre-integration.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC irradiancePsoDesc;

		ZeroMemory(&irradiancePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		irradiancePsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		irradiancePsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		irradiancePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		irradiancePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		irradiancePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		irradiancePsoDesc.SampleMask = UINT_MAX;
		irradiancePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		irradiancePsoDesc.NumRenderTargets = 1;
		irradiancePsoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
		irradiancePsoDesc.SampleDesc.Count = 1;
		irradiancePsoDesc.SampleDesc.Quality = 0;
		irradiancePsoDesc.DSVFormat = mDepthStencilFormat;

		// The camera is inside the sky sphere, so just turn off culling.
		irradiancePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		// Make sure the depth function is LESS_EQUAL and not just LESS.  
		// Otherwise, the normalized depth values at z = 1 (NDC) will 
		// fail the depth test if the depth buffer was cleared to 1.
#if USE_REVERSE_Z
		irradiancePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		irradiancePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		irradiancePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		irradiancePsoDesc.pRootSignature = mRootSignatures["Sky"].Get();
		irradiancePsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\SkyVS.cso");
		irradiancePsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\IrradianceCubemapPS.cso");
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&irradiancePsoDesc, IID_PPV_ARGS(&mPSOs["Irradiance"])));
	}

	// PSO for prefilter pre-integration.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC prefilterPsoDesc;

		ZeroMemory(&prefilterPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		prefilterPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		prefilterPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		prefilterPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		prefilterPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		prefilterPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		prefilterPsoDesc.SampleMask = UINT_MAX;
		prefilterPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		prefilterPsoDesc.NumRenderTargets = 1;
		prefilterPsoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prefilterPsoDesc.SampleDesc.Count = 1;
		prefilterPsoDesc.SampleDesc.Quality = 0;
		prefilterPsoDesc.DSVFormat = mDepthStencilFormat;

		// The camera is inside the sky sphere, so just turn off culling.
		prefilterPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		// Make sure the depth function is LESS_EQUAL and not just LESS.  
		// Otherwise, the normalized depth values at z = 1 (NDC) will 
		// fail the depth test if the depth buffer was cleared to 1.
#if USE_REVERSE_Z
		prefilterPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		prefilterPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		prefilterPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		prefilterPsoDesc.pRootSignature = mRootSignatures["Sky"].Get();
		prefilterPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\SkyVS.cso");
		prefilterPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\PrefilterCubemapPS.cso");
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&prefilterPsoDesc, IID_PPV_ARGS(&mPSOs["Prefilter"])));
	}

}

void GDxRenderer::BuildFrameResources()
{
	for (int i = 0; i < NUM_FRAME_RESOURCES; ++i)
	{
		mFrameResources.push_back(std::make_unique<GDxFrameResource>(md3dDevice.Get(),
			2, MAX_SCENE_OBJECT_NUM, MAX_MATERIAL_NUM));//(UINT)pSceneObjects.size(), (UINT)pMaterials.size()));
	}

	for (auto i = 0u; i < (6 * mPrefilterLevels); i++)
	{
		PreIntegrationPassCbs.push_back(std::make_unique<GDxUploadBuffer<SkyPassConstants>>(md3dDevice.Get(), 1, true));
	}
}

void GDxRenderer::CubemapPreIntegration()
{
	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	//
	// Irradiance cubemap pre-integration
	//

	// Reset root parameters and PSO.
	mCommandList->RSSetViewports(1, &mCubeRtvs["Irradiance"]->mViewport);
	mCommandList->RSSetScissorRects(1, &mCubeRtvs["Irradiance"]->mScissorRect);
	
	mCommandList->SetGraphicsRootSignature(mRootSignatures["Sky"].Get());

	mCommandList->SetPipelineState(mPSOs["Irradiance"].Get());

	// Load object CB.
	mCurrFrameResource = mFrameResources[0].get();
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : pSceneObjects)
	{
		e.second->UpdateTransform();

		/*
		auto dxTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTransform());
		if (dxTrans == nullptr)
			ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");

		auto dxTexTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTexTransform());
		if (dxTexTrans == nullptr)
			ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");
		*/

		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		XMMATRIX world = GDx::GGiToDxMatrix(e.second->GetTransform());
		XMMATRIX texTransform = GDx::GGiToDxMatrix(e.second->GetTexTransform());

		ObjectConstants objConstants;
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.PrevWorld, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.InvTransWorld, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
		//objConstants.MaterialIndex = e.second->GetMaterial()->MatIndex;
		//objConstants.ObjPad0 = 0;
		//objConstants.ObjPad1 = 0;
		//objConstants.ObjPad2 = 0;

		currObjectCB->CopyData(e.second->GetObjIndex(), objConstants);
	}

	// Load sky pass CB.
	for (auto i = 0u; i < mPrefilterLevels; i++)
	{
		for (auto j = 0u; j < 6u; j++)
		{
			auto view = pCubemapSampleCamera[j]->GetView();
			auto proj = pCubemapSampleCamera[j]->GetProj();
			GGiFloat4x4 viewProj = view * proj;

			viewProj.Transpose();
			XMStoreFloat4x4(&mSkyPassCB.ViewProj, GDx::GGiToDxMatrix(viewProj));
			XMStoreFloat4x4(&mSkyPassCB.PrevViewProj, GDx::GGiToDxMatrix(viewProj));
			mSkyPassCB.pad1 = 0.0f;
			auto eyePos = pCamera->GetPosition();
			mSkyPassCB.EyePosW = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
			mSkyPassCB.PrevPos = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
			if (i == 0)
			{
				mSkyPassCB.roughness = 0.01f;
			}
			else
			{
				mSkyPassCB.roughness = ((float)i / (float)mPrefilterLevels);
			}
			auto uploadCB = PreIntegrationPassCbs[i * 6 + j].get();
			uploadCB->CopyData(0, mSkyPassCB);
		}
	}

	for (auto i = 0u; i < 6; i++)
	{
		mCommandList->ClearRenderTargetView(mCubeRtvs["Irradiance"]->mRtvHeap.handleCPU(i), Colors::LightSteelBlue, 0, nullptr);

		auto skyCB = PreIntegrationPassCbs[i]->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, skyCB->GetGPUVirtualAddress());

		CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(2, skyTexDescriptor);

		mCommandList->OMSetRenderTargets(1, &(mCubeRtvs["Irradiance"]->mRtvHeap.handleCPU(i)), true, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::Sky, true, false);
	}

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Irradiance"]->mResource.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

	//
	// Prefilter cubemap pre-integration
	//

	// Reset root parameters and PSO.
	mCommandList->SetGraphicsRootSignature(mRootSignatures["Sky"].Get());

	mCommandList->SetPipelineState(mPSOs["Prefilter"].Get());

	for (auto i = 0u; i < mPrefilterLevels; i++)
	{
		for (auto j = 0u; j < 6; j++)
		{
			mCommandList->RSSetViewports(1, &mCubeRtvs["Prefilter_" + std::to_string(i)]->mViewport);
			mCommandList->RSSetScissorRects(1, &mCubeRtvs["Prefilter_" + std::to_string(i)]->mScissorRect);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			mCommandList->ClearRenderTargetView(mCubeRtvs["Prefilter_" + std::to_string(i)]->mRtvHeap.handleCPU(j), Colors::LightSteelBlue, 0, nullptr);

			//SetPassCbByCamera(PreIntegrationPassCB[i].get(), 0.0f, 0.0f, mCubemapSampleCamera[i]);
			auto passCB = PreIntegrationPassCbs[i * 6 + j]->Resource();
			mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

			CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
			skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
			mCommandList->SetGraphicsRootDescriptorTable(2, skyTexDescriptor);

			mCommandList->OMSetRenderTargets(1, &(mCubeRtvs["Prefilter_" + std::to_string(i)]->mRtvHeap.handleCPU(j)), true, nullptr);

			DrawSceneObjects(mCommandList.Get(), RenderLayer::Sky, true, false);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}
	}
}

/*
void GDxRenderer::SaveBakedCubemap(std::wstring workDir, std::wstring CubemapPath)
{
	std::wstring originalPath = workDir + CubemapPath;
	std::wstring savePathPrefix = originalPath.substr(0, originalPath.rfind(L"."));

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Irradiance"]->mResource.Get(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));
	ThrowIfFailed(
		DirectX::SaveDDSTextureToFile(mCommandQueue.Get(),
			mCubeRtvs["Irradiance"]->mResource.Get(),
			(savePathPrefix + L"_Irradiance.dds").c_str(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE)
	);
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Irradiance"]->mResource.Get(),
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
	
	for (auto i = 0u; i < mPrefilterLevels; i++)
	{
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));
		ThrowIfFailed(
			DirectX::SaveDDSTextureToFile(mCommandQueue.Get(),
				mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
				(savePathPrefix + L"_Prefilter_" + std::to_wstring(i) + L".dds").c_str(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE)
		);
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
	}
}
*/

void GDxRenderer::CreateRendererFactory()
{
	GDxRendererFactory fac(md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get());
	mFactory = std::make_unique<GDxRendererFactory>(fac);
}

void GDxRenderer::CreateFilmboxManager()
{
	mFilmboxManager = std::make_unique<GDxFilmboxManager>();
	mFilmboxManager->SetRendererFactory(mFactory.get());
}

void GDxRenderer::CreateCommandObjects()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue)));

	ThrowIfFailed(md3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(mDirectCmdListAlloc.GetAddressOf())));

	ThrowIfFailed(md3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc.Get(), // Associated command allocator
		nullptr,                   // Initial PipelineStateObject
		IID_PPV_ARGS(mCommandList.GetAddressOf())));

	// Start off in a closed state.  This is because the first time we refer 
	// to the command list we will Reset it, and it needs to be closed before
	// calling Reset.
	mCommandList->Close();
}

void GDxRenderer::CreateSwapChain()
{
	// Release the previous swapchain we will be recreating.
	mSwapChain.Reset();

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = mClientWidth;
	sd.BufferDesc.Height = mClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.OutputWindow = mhMainWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	// Note: Swap chain uses queue to perform flush.
	ThrowIfFailed(mdxgiFactory->CreateSwapChain(
		mCommandQueue.Get(),
		&sd,
		mSwapChain.GetAddressOf()));
}

void GDxRenderer::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +1 for screen normal map, +2 for ambient maps.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// Add +1 DSV for shadow map.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 2;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void GDxRenderer::InitializeGpuProfiler()
{
	GDxGpuProfiler::GetGpuProfiler().Initialize(md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get());
}

#pragma endregion

#pragma region Draw

void GDxRenderer::DrawSceneObjects(ID3D12GraphicsCommandList* cmdList, const RenderLayer layer, bool bSetObjCb, bool bSetSubmeshCb, bool bCheckCullState)
{
	// For each render item...
	for (size_t i = 0; i < pSceneObjectLayer[((int)layer)].size(); ++i)
	{
		auto sObject = pSceneObjectLayer[((int)layer)][i];
		if (!bCheckCullState || (bCheckCullState && (sObject->GetCullState() == CullState::Visible)))
			DrawSceneObject(cmdList, sObject, bSetObjCb, bSetSubmeshCb);
	}
}

void GDxRenderer::DrawSceneObject(ID3D12GraphicsCommandList* cmdList, GRiSceneObject* sObject, bool bSetObjCb, bool bSetSubmeshCb, bool bCheckCullState)
{
	GDxSceneObject* dxSO = dynamic_cast<GDxSceneObject*>(sObject);
	if (dxSO == NULL)
	{
		ThrowGGiException("Cast failed : from GRiSceneObject* to GDxSceneObject*.")
	}

	GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(sObject->GetMesh());
	if (dxMesh == NULL)
	{
		ThrowGGiException("Cast failed : from GRiMesh* to GDxMesh*.")
	}

	cmdList->IASetVertexBuffers(0, 1, &dxMesh->mVIBuffer->VertexBufferView());
	cmdList->IASetIndexBuffer(&dxMesh->mVIBuffer->IndexBufferView());
	cmdList->IASetPrimitiveTopology(dxSO->GetPrimitiveTopology());

	if (bSetObjCb)
	{
		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();
		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + sObject->GetObjIndex() * objCBByteSize;
		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
	}

	if (!bCheckCullState || (bCheckCullState && (sObject->GetCullState() == CullState::Visible)))
	{
		//cmdList->DrawIndexedInstanced(dxMesh->mVIBuffer->IndexCount, 1, 0, 0, 0);
		for (auto& submesh : dxMesh->Submeshes)
		{
			if (bSetSubmeshCb)
			{
				auto overrideMat = dxSO->GetOverrideMaterial(submesh.first);
				if (overrideMat != nullptr)
					cmdList->SetGraphicsRoot32BitConstants(1, 1, &(overrideMat->MatIndex), 0);
				else
					cmdList->SetGraphicsRoot32BitConstants(1, 1, &(submesh.second.GetMaterial()->MatIndex), 0);
			}
			cmdList->DrawIndexedInstanced(submesh.second.IndexCount, 1, submesh.second.StartIndexLocation, submesh.second.BaseVertexLocation, 0);
		}
	}
}

#pragma endregion

#pragma region Runtime

void GDxRenderer::RegisterTexture(GRiTexture* text)
{
	GDxTexture* dxTex = dynamic_cast<GDxTexture*>(text);
	if (dxTex == nullptr)
		ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.Format = dxTex->Resource->GetDesc().Format;
	srvDesc.Texture2D.MipLevels = dxTex->Resource->GetDesc().MipLevels;

	// if srv is previously created
	if (dxTex->texIndex != -1)
	{
		md3dDevice->CreateShaderResourceView(dxTex->Resource.Get(), &srvDesc, GetCpuSrv(mTextrueHeapIndex + dxTex->texIndex));
	}
	else
	{
		if (mTexturePoolFreeIndex.empty())
			ThrowGGiException("Texture pool has run out.");
		auto it = mTexturePoolFreeIndex.begin();
		dxTex->texIndex = *it;
		md3dDevice->CreateShaderResourceView(dxTex->Resource.Get(), &srvDesc, GetCpuSrv(mTextrueHeapIndex + *it));
		mTexturePoolFreeIndex.erase(it);
	}
}

GRiSceneObject* GDxRenderer::SelectSceneObject(int sx, int sy)
{
	GGiFloat4x4 P = pCamera->GetProj();

	// Compute picking ray in view space.
	float vx = (+2.0f*sx / mClientWidth - 1.0f) / P.GetElement(0, 0);
	float vy = (-2.0f*sy / mClientHeight + 1.0f) / P.GetElement(1, 1);

	// Ray definition in view space.
	XMVECTOR viewRayOrigin = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	XMVECTOR viewRayDir = XMVectorSet(vx, vy, 1.0f, 0.0f);

	XMMATRIX dxView = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(dxView), dxView);

	GRiSceneObject* pickedSceneObject = nullptr;
	float tPicked = GGiEngineUtil::Infinity;

	// Check if we picked an opaque render item.  A real app might keep a separate "picking list"
	// of objects that can be selected.   
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		auto mesh = so->GetMesh();

		XMMATRIX W = GDx::GGiToDxMatrix(so->GetTransform());

		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(W), W);

		// Tranform ray to vi space of Mesh.
		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

		XMVECTOR rayOrigin = XMVector3TransformCoord(viewRayOrigin, toLocal);
		XMVECTOR rayDir = XMVector3TransformNormal(viewRayDir, toLocal);

		// Make the ray direction unit length for the intersection tests.
		rayDir = XMVector3Normalize(rayDir);

		// If we hit the bounding box of the Mesh, then we might have picked a Mesh triangle,
		// so do the ray/triangle tests.
		//
		// If we did not hit the bounding box, then it is impossible that we hit 
		// the Mesh, so do not waste effort doing ray/triangle tests.
		BoundingBox bBox;
		bBox.Center.x = /*so->GetLocation()[0] +*/ so->GetMesh()->bounds.Center[0];
		bBox.Center.y = /*so->GetLocation()[1] +*/ so->GetMesh()->bounds.Center[1];
		bBox.Center.z = /*so->GetLocation()[2] +*/ so->GetMesh()->bounds.Center[2];
		bBox.Extents.x = so->GetMesh()->bounds.Extents[0];
		bBox.Extents.y = so->GetMesh()->bounds.Extents[1];
		bBox.Extents.z = so->GetMesh()->bounds.Extents[2];
		float tmin = 0.0f;
		if (bBox.Intersects(rayOrigin, rayDir, tmin))
		{
			// NOTE: For the demo, we know what to cast the vertex/index data to.  If we were mixing
			// formats, some metadata would be needed to figure out what to cast it to.
			GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(so->GetMesh());
			if (dxMesh == nullptr)
				ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");
			shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
			if (dxViBuffer == nullptr)
				ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");
			
			auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
			auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
			UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;

			// Find the nearest ray/triangle intersection.
			tmin = GGiEngineUtil::Infinity;
			for (auto submesh : so->GetMesh()->Submeshes)
			{
				auto startIndexLocation = submesh.second.StartIndexLocation;
				auto baseVertexLocation = submesh.second.BaseVertexLocation;

				for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
				{
					// Indices for this triangle.
					UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;
					UINT i1 = indices[startIndexLocation + i * 3 + 1] + baseVertexLocation;
					UINT i2 = indices[startIndexLocation + i * 3 + 2] + baseVertexLocation;

					// Vertices for this triangle.
					XMFLOAT3 v0f;
					XMFLOAT3 v1f;
					XMFLOAT3 v2f;
					v0f.x = vertices[i0].Position[0];
					v0f.y = vertices[i0].Position[1];
					v0f.z = vertices[i0].Position[2];
					v1f.x = vertices[i1].Position[0];
					v1f.y = vertices[i1].Position[1];
					v1f.z = vertices[i1].Position[2];
					v2f.x = vertices[i2].Position[0];
					v2f.y = vertices[i2].Position[1];
					v2f.z = vertices[i2].Position[2];
					XMVECTOR v0 = XMLoadFloat3(&v0f);
					XMVECTOR v1 = XMLoadFloat3(&v1f);
					XMVECTOR v2 = XMLoadFloat3(&v2f);

					// We have to iterate over all the triangles in order to find the nearest intersection.
					float t = 0.0f;
					if (TriangleTests::Intersects(rayOrigin, rayDir, v0, v1, v2, t))
					{
						if (t < tmin)
						{
							// This is the new nearest picked triangle.
							tmin = t;
						}
					}
				}
			}

			std::vector<float> soScale = so->GetScale();
			float relSize = (float)pow(soScale[0] * soScale[0] + soScale[1] * soScale[1] + soScale[2] * soScale[2], 0.5);
			tmin *= relSize;

			if (tmin < tPicked)
			{
				tPicked = tmin;
				pickedSceneObject = so;
			}
		}
	}
	return pickedSceneObject;
}

std::vector<ProfileData> GDxRenderer::GetGpuProfiles()
{
	return GDxGpuProfiler::GetGpuProfiler().GetProfiles();
}

void GDxRenderer::BuildMeshSDF()
{
	std::vector<std::shared_ptr<GRiKdPrimitive>> prims;
	prims.clear();

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor = GetCpuSrv(mSdfTextrueIndex);

	int sdfIndex = 0;

	mMeshSdfDescriptorBuffer = std::make_unique<GDxUploadBuffer<MeshSdfDescriptor>>(md3dDevice.Get(), MAX_MESH_NUM, true);

	for (auto mesh : pMeshes)
	{
		if (mesh.second->Name == L"Box" ||
			mesh.second->Name == L"Sphere" ||
			mesh.second->Name == L"Cylinder" ||
			mesh.second->Name == L"Grid" ||
			mesh.second->Name == L"Quad" ||
			mesh.second->Name == L"Cerberus"||
			mesh.second->Name != L"Stool"
			)
			continue;

		GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(mesh.second);
		if (dxMesh == nullptr)
			ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");
		shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
		if (dxViBuffer == nullptr)
			ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");

		auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
		auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
		UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;

		// Collect primitives.
		for (auto &submesh : dxMesh->Submeshes)
		{
			auto startIndexLocation = submesh.second.StartIndexLocation;
			auto baseVertexLocation = submesh.second.BaseVertexLocation;

			for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
			{
				// Indices for this triangle.
				UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;

				auto prim = std::make_shared<GRiKdPrimitive>(&vertices[i0], &vertices[i0 + 1], &vertices[i0 + 2]);

				prims.push_back(prim);
			}
		}

		int isectCost = 80;
		int travCost = 1;
		float emptyBonus = 0.5f;
		int maxPrims = 1;
		int maxDepth = -1;

		auto pAcceleratorTree = std::make_shared<GRiKdTree>(std::move(prims), isectCost, travCost, emptyBonus,
			maxPrims, maxDepth);

		dxMesh->SetSdfResolution(64);
		auto sdfRes = dxMesh->GetSdfResolution();
		auto sdf = std::vector<float>(sdfRes * sdfRes * sdfRes);

		float maxExtent = 0.0f;
		for (int dim = 0; dim < 3; dim++)
		{
			float range = abs(dxMesh->bounds.Center[dim] + dxMesh->bounds.Extents[dim]);
			if (range > maxExtent)
				maxExtent = range;
			range = abs(dxMesh->bounds.Center[dim] - dxMesh->bounds.Extents[dim]);
			if (range > maxExtent)
				maxExtent = range;
		}
		auto sdfExtent = maxExtent * 1.4f * 2.0f;// 1.4f * dxMesh->bounds.Extents[dxMesh->bounds.MaximumExtent()] * 2.0f;
		auto sdfUnit = sdfExtent / (float)sdfRes;
		auto initMinDisFront = 1.414f * sdfExtent;
		auto initMaxDisBack = -1.414f * sdfExtent;

		for (int z = 0; z < sdfRes; z++)
		{
			for (int y = 0; y < sdfRes; y++)
			{
				for (int x = 0; x < sdfRes; x++)
				{
					mRendererThreadPool->Enqueue([&, x, y, z]
						{
							int index = z * sdfRes * sdfRes + y * sdfRes + x;
							sdf[index] = 0.0f;

							GGiFloat3 rayOrigin(
								((float)x - sdfRes / 2 + 0.5f) * sdfUnit,
								((float)y - sdfRes / 2 + 0.5f) * sdfUnit,
								((float)z - sdfRes / 2 + 0.5f) * sdfUnit
							);

							static int rayNum = 128;
							static float fibParam = 2 * GGiEngineUtil::PI * 0.618f;
							float fibInter = 0.0f;
							GRiRay ray;
							float minDist = initMinDisFront;
							float outDis = 999.0f;
							int numFront = 0;
							int numBack = 0;
							bool bBackFace;

							ray.Origin[0] = rayOrigin.x;
							ray.Origin[1] = rayOrigin.y;
							ray.Origin[2] = rayOrigin.z;

							// Fibonacci lattices.
							for (int n = 0; n < rayNum; n++)
							{
								ray.Direction[1] = (float)(2 * n + 1) / (float)rayNum - 1;
								fibInter = sqrt(1.0f - ray.Direction[1] * ray.Direction[1]);
								ray.Direction[0] = fibInter * cos(fibParam * n);
								ray.Direction[2] = fibInter * sin(fibParam * n);

								ray.tMax = 99999.0f;

								if (pAcceleratorTree->IntersectDis(ray, &outDis, bBackFace))
								{
									if (bBackFace)
									{
										numBack++;
									}
									else
									{
										numFront++;
									}
									if (outDis < minDist)
										minDist = outDis;
								}
							}

							sdf[index] = minDist;
							if (numBack > numFront)
								sdf[index] *= -1;
						}
					);
				}
			}
		}

		mRendererThreadPool->Flush();

		dxMesh->InitializeSdf(sdf);

		ResetCommandList();

		//Microsoft::WRL::ComPtr<ID3D12Resource> sdfTexture = nullptr;
		//Microsoft::WRL::ComPtr<ID3D12Resource> sdfTextureUploadBuffer = nullptr;
		D3D12_RESOURCE_DESC texDesc;
		ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		texDesc.Alignment = 0;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		texDesc.Width = (UINT)(sdfRes);
		texDesc.Height = (UINT)(sdfRes);
		texDesc.DepthOrArraySize = (UINT)(sdfRes);
		texDesc.Format = DXGI_FORMAT::DXGI_FORMAT_R32_FLOAT;

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&mSdfTextures[sdfIndex])));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(mSdfTextures[sdfIndex].Get(), 0, 1);

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&mSdfTextureUploadBuffer[sdfIndex])));

		D3D12_SUBRESOURCE_DATA textureData = {};
		textureData.pData = sdf.data();
		textureData.RowPitch = static_cast<LONG_PTR>((4 * sdfRes));
		textureData.SlicePitch = textureData.RowPitch * sdfRes;

		UpdateSubresources(mCommandList.Get(), mSdfTextures[sdfIndex].Get(), mSdfTextureUploadBuffer[sdfIndex].Get(), 0, 0, 1, &textureData);
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSdfTextures[sdfIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		D3D12_SHADER_RESOURCE_VIEW_DESC sdfSrvDesc = {};
		sdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sdfSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		sdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		sdfSrvDesc.Texture3D.MipLevels = 1;
		md3dDevice->CreateShaderResourceView(mSdfTextures[sdfIndex].Get(), &sdfSrvDesc, hDescriptor);
		hDescriptor.Offset(mCbvSrvUavDescriptorSize);

		dxMesh->mSdfIndex = sdfIndex;
		mMeshSdfDescriptors[sdfIndex].HalfExtent = 0.5f * sdfExtent;
		mMeshSdfDescriptors[sdfIndex].Radius = 0.707f * sdfExtent;
		mMeshSdfDescriptors[sdfIndex].Resolution = sdfRes;
		sdfIndex++;

		ExecuteCommandList();
	}

	/*
	int soSdfIndex = 0;
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		if (so->GetMesh()->GetSdf()->size() > 0)
		{
			mSceneObjectSdfDescriptors[soSdfIndex].SdfIndex = so->GetMesh()->mSdfIndex;
			XMMATRIX trans = GDx::GGiToDxMatrix(so->GetTransform());
			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].Transform, trans);
			soSdfIndex++;
		}
	}
	mSceneObjectSdfNum = soSdfIndex;
	*/

	auto meshSdfBuffer = mMeshSdfDescriptorBuffer.get();
	meshSdfBuffer->CopyData(0, mMeshSdfDescriptors[0]);
	//auto soSdfBuffer = mSceneObjectSdfDescriptorBuffer.get();
	//soSdfBuffer->CopyData(0, mSceneObjectSdfDescriptors[0]);
	
	/*
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		auto mesh = so->GetMesh();

		XMMATRIX soWorld = GDx::GGiToDxMatrix(so->GetTransform());

		GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(so->GetMesh());
		if (dxMesh == nullptr)
			ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");
		shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
		if (dxViBuffer == nullptr)
			ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");

		auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
		auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
		UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;
 
		for (auto &submesh : so->GetMesh()->Submeshes)
		{
			auto startIndexLocation = submesh.second.StartIndexLocation;
			auto baseVertexLocation = submesh.second.BaseVertexLocation;

			for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
			{
				// Indices for this triangle.
				UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;
				//UINT i1 = indices[startIndexLocation + i * 3 + 1] + baseVertexLocation;
				//UINT i2 = indices[startIndexLocation + i * 3 + 2] + baseVertexLocation;

				GRiVertex verticesWorld[3];
				auto vert1 = XMFLOAT4(vertices[i0].Position[0], vertices[i0].Position[1], vertices[i0].Position[2], 1.0f);
				auto vert2 = XMFLOAT4(vertices[i0 + 1].Position[0], vertices[i0 + 1].Position[1], vertices[i0 + 1].Position[2], 1.0f);
				auto vert3 = XMFLOAT4(vertices[i0 + 2].Position[0], vertices[i0 + 2].Position[1], vertices[i0 + 2].Position[2], 1.0f);
				auto vertVec1 = XMLoadFloat4(&vert1);
				auto vertVec2 = XMLoadFloat4(&vert2);
				auto vertVec3 = XMLoadFloat4(&vert3);
				auto vert1WorldPos = DirectX::XMVector4Transform(vertVec1, soWorld);
				auto vert2WorldPos = DirectX::XMVector4Transform(vertVec2, soWorld);
				auto vert3WorldPos = DirectX::XMVector4Transform(vertVec3, soWorld);
				GRiVertex vert1World, vert2World, vert3World;
				vert1World.Position[0] = vert1WorldPos.m128_f32[0];
				vert1World.Position[1] = vert1WorldPos.m128_f32[1];
				vert1World.Position[2] = vert1WorldPos.m128_f32[2];
				vert2World.Position[0] = vert2WorldPos.m128_f32[0];
				vert2World.Position[1] = vert2WorldPos.m128_f32[1];
				vert2World.Position[2] = vert2WorldPos.m128_f32[2];
				vert3World.Position[0] = vert3WorldPos.m128_f32[0];
				vert3World.Position[1] = vert3WorldPos.m128_f32[1];
				vert3World.Position[2] = vert3WorldPos.m128_f32[2];
				verticesWorld[0] = vert1World;
				verticesWorld[1] = vert2World;
				verticesWorld[2] = vert3World;

				auto prim = std::make_shared<GRiKdPrimitive>(verticesWorld[0], verticesWorld[1], verticesWorld[2]);

				prims.push_back(prim);
			}
		}
	}
	int isectCost = 80;
	int travCost = 1;
	float emptyBonus = 0.5f;
	int maxPrims = 1;
	int maxDepth = -1;

	auto mAcceleratorTree = std::make_shared<GRiKdTree>(std::move(prims), isectCost, travCost, emptyBonus,
		maxPrims, maxDepth);

	GRiRay ray;
	ray.Direction[0] = 0.0f;
	ray.Direction[1] = -1.0f;
	ray.Direction[2] = 0.0f;
	ray.Origin[0] = 0.0f;
	ray.Origin[1] = 100.0f;
	ray.Origin[2] = 0.0f;
	float distance = -1.0f;
	bool bBackface = false;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//80

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = 0.0f;
	ray.Direction[2] = 1.0f;
	ray.Origin[0] = 0.0f;
	ray.Origin[1] = 70.0f;
	ray.Origin[2] = 0.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//~100

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = -1.0f;
	ray.Direction[2] = 0.0f;
	ray.Origin[0] = -80.0f;
	ray.Origin[1] = 65.0f;
	ray.Origin[2] = -80.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//15

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = 1.0f;
	ray.Direction[2] = 0.0f;
	ray.Origin[0] = 100.0f;
	ray.Origin[1] = 0.0f;
	ray.Origin[2] = 0.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//-25

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = 0.717f;
	ray.Direction[2] = 0.717f;
	ray.Origin[0] = 100.0f;
	ray.Origin[1] = 0.0f;
	ray.Origin[2] = 0.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//-35
	*/
}

#pragma endregion

#pragma region Util

bool GDxRenderer::IsRunning()
{
	if (md3dDevice)
		return true;
	else
		return false;
}

ID3D12Resource* GDxRenderer::CurrentBackBuffer()const
{
	return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE GDxRenderer::CurrentBackBufferView()const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE GDxRenderer::DepthStencilView()const
{
	return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void GDxRenderer::LogAdapters()
{
	UINT i = 0;
	IDXGIAdapter* adapter = nullptr;
	std::vector<IDXGIAdapter*> adapterList;
	while (mdxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		std::wstring text = L"***Adapter: ";
		text += desc.Description;
		text += L"\n";

		OutputDebugString(text.c_str());

		adapterList.push_back(adapter);

		++i;
	}

	for (size_t i = 0; i < adapterList.size(); ++i)
	{
		LogAdapterOutputs(adapterList[i]);
		ReleaseCom(adapterList[i]);
	}
}

void GDxRenderer::LogAdapterOutputs(IDXGIAdapter* adapter)
{
	UINT i = 0;
	IDXGIOutput* output = nullptr;
	while (adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_OUTPUT_DESC desc;
		output->GetDesc(&desc);

		std::wstring text = L"***Output: ";
		text += desc.DeviceName;
		text += L"\n";
		OutputDebugString(text.c_str());

		LogOutputDisplayModes(output, mBackBufferFormat);

		ReleaseCom(output);

		++i;
	}
}

void GDxRenderer::LogOutputDisplayModes(IDXGIOutput* output, DXGI_FORMAT format)
{
	UINT count = 0;
	UINT flags = 0;

	// Call with nullptr to get list count.
	output->GetDisplayModeList(format, flags, &count, nullptr);

	std::vector<DXGI_MODE_DESC> modeList(count);
	output->GetDisplayModeList(format, flags, &count, &modeList[0]);

	for (auto& x : modeList)
	{
		UINT n = x.RefreshRate.Numerator;
		UINT d = x.RefreshRate.Denominator;
		std::wstring text =
			L"Width = " + std::to_wstring(x.Width) + L" " +
			L"Height = " + std::to_wstring(x.Height) + L" " +
			L"Refresh = " + std::to_wstring(n) + L"/" + std::to_wstring(d) +
			L"\n";

		::OutputDebugString(text.c_str());
	}
}

void GDxRenderer::FlushCommandQueue()
{
	// Advance the fence value to mark commands up to this fence point.
	mCurrentFence++;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));

	// Wait until the GPU has completed commands up to this fence point.
	if (mFence->GetCompletedValue() < mCurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

		// Fire event when GPU hits current fence.  
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void GDxRenderer::ResetCommandList()
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
	ThrowIfFailed(cmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["GBuffer"].Get()));
}

void GDxRenderer::ExecuteCommandList()
{
	// Execute the commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until command queue is complete.
	FlushCommandQueue();
}

CD3DX12_CPU_DESCRIPTOR_HANDLE GDxRenderer::GetCpuSrv(int index)const
{
	auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE GDxRenderer::GetGpuSrv(int index)const
{
	auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE GDxRenderer::GetDsv(int index)const
{
	auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
	dsv.Offset(index, mDsvDescriptorSize);
	return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE GDxRenderer::GetRtv(int index)const
{
	auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtv.Offset(index, mRtvDescriptorSize);
	return rtv;
}

#pragma endregion
