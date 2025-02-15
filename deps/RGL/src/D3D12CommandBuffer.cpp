#if RGL_DX12_AVAILABLE
#include "D3D12CommandBuffer.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12Synchronization.hpp"
#include <directx/d3dx12.h>
#include "D3D12Texture.hpp"
#include "D3D12RenderPipeline.hpp"
#include "D3D12Buffer.hpp"
#include "D3D12Device.hpp"
#include "D3D12Sampler.hpp"
#include "D3D12RenderPass.hpp"
#include "D3D12ComputePipeline.hpp"

#if __has_include(<pix3.h>)
#include <pix3.h>
#define PIX_SUPPORTED
#endif

namespace RGL {

	CommandBufferD3D12::CommandBufferD3D12(decltype(owningQueue) owningQueue) : owningQueue(owningQueue)
	{
		commandList = owningQueue->CreateCommandList();
		DX_CHECK(owningQueue->m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&internalFence)));
		internalFenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	}
	void CommandBufferD3D12::Reset()
	{
		ID3D12CommandAllocator* commandAllocator;
		UINT dataSize = sizeof(commandAllocator);
		DX_CHECK(commandList->GetPrivateData(__uuidof(ID3D12CommandAllocator), &dataSize, &commandAllocator));
		if (!ended) {
			DX_CHECK(commandList->Close());
		}
		DX_CHECK(commandAllocator->Reset());	// gotta reset this too, otherwise we leak
		DX_CHECK(commandList->Reset(commandAllocator, nullptr));
		ended = false;
		owningQueue->m_d3d12CommandQueue->Signal(internalFence.Get(), 0);
	}
	void CommandBufferD3D12::Begin()
	{

	}
	void CommandBufferD3D12::End()
	{
		// put all the resources back in their native states
		auto nItems = activeBuffers.size() + activeTextures.size();
		if (nItems > 0) {
			stackarray(barriers, CD3DX12_RESOURCE_BARRIER, nItems);
			int i = 0;
			for (const auto& [resource, record] : activeBuffers) {
				if (record.state == resource->nativeState) {
					continue;	// states must be different.
				}
				barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
					resource->GetResource(),
					record.state,
					resource->nativeState
				);
				i++;
			}
			// all the textures
			for (const auto& [resource, record] : activeTextures) {
				if (record.state == resource.texture->nativeState) {
					continue;	// states must be different.
				}
				barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
					resource.texture->GetResource(),
					record.state,
					resource.texture->nativeState,
					resource.mip
				);
				i++;
			}

			if (i > 0) {
				commandList->ResourceBarrier(i, barriers);
			}
		}


		DX_CHECK(commandList->Close());
		ended = true;
		activeBuffers.clear();
		activeTextures.clear();
	}
	void CommandBufferD3D12::BeginRendering(RGLRenderPassPtr renderPass)
	{
		currentRenderPass = std::static_pointer_cast<RenderPassD3D12>(renderPass);

		const auto nrtvs = currentRenderPass->config.attachments.size();
		stackarray(rtvs, CD3DX12_CPU_DESCRIPTOR_HANDLE,nrtvs);

		uint32_t i = 0;
		for (const auto& attachment : currentRenderPass->config.attachments) {
			auto& tx = currentRenderPass->textures[i].texture.dx;

			SyncIfNeeded(tx, D3D12_RESOURCE_STATE_RENDER_TARGET);

			Assert(tx.rtvAllocated(),"This texture was not allocated as a render target!");
			
			auto rtv = tx.parentResource->owningDevice->RTVHeap->GetCpuHandle(tx.rtvIDX);

			if (currentRenderPass->config.attachments[i].loadOp == RGL::LoadAccessOperation::Clear) {
				commandList->ClearRenderTargetView(rtv, attachment.clearColor.data(), 0, nullptr);
			}


			rtvs[i] = rtv;

			i++;
		}

		//depth stencil
		D3D12_CPU_DESCRIPTOR_HANDLE dsv;
		D3D12_CPU_DESCRIPTOR_HANDLE* dsvptr = nullptr;
		{
			if (currentRenderPass->depthTexture) {
				auto& tx = currentRenderPass->depthTexture.value().texture.dx;
				SyncIfNeeded(tx, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				Assert(tx.dsvAllocated(), "Texture was not allocated as a depth stencil!");
				dsv = tx.parentResource->owningDevice->DSVHeap->GetCpuHandle(tx.dsvIDX);
				dsvptr = &dsv;
				if (currentRenderPass->config.depthAttachment->loadOp == LoadAccessOperation::Clear) {
					commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, currentRenderPass->config.depthAttachment->clearColor[0], 0, 0, nullptr);
				}
			}
		}

		// bind the targets
		commandList->OMSetRenderTargets(nrtvs, rtvs, FALSE, dsvptr);
	}
	void CommandBufferD3D12::EndRendering()
	{
		currentRenderPass = nullptr;
		currentRenderPipeline = nullptr;
	}
	void CommandBufferD3D12::BindRenderPipeline(RGLRenderPipelinePtr in_pipeline)
	{
		currentRenderPipeline = std::static_pointer_cast<RenderPipelineD3D12>(in_pipeline);
		commandList->SetPipelineState(currentRenderPipeline->pipelineState.Get());
		commandList->SetGraphicsRootSignature(currentRenderPipeline->pipelineLayout->rootSignature.Get());
		commandList->IASetPrimitiveTopology(currentRenderPipeline->overrideMode);
	}
	void CommandBufferD3D12::BeginCompute(RGLComputePipelinePtr in_pipeline)
	{
		currentComputePipeline = std::static_pointer_cast<ComputePipelineD3D12>(in_pipeline);
		commandList->SetPipelineState(currentComputePipeline->pipelineState.Get());
		commandList->SetComputeRootSignature(currentComputePipeline->pipelineLayout->rootSignature.Get());
	}
	void CommandBufferD3D12::EndCompute()
	{
		currentComputePipeline.reset();
	}
	void CommandBufferD3D12::DispatchCompute(uint32_t threadsX, uint32_t threadsY, uint32_t threadsZ,  uint32_t threadsPerThreadgroupX, uint32_t threadsPerThreadgroupY, uint32_t threadsPerThreadgroupZ)
	{
		commandList->Dispatch(threadsX, threadsY, threadsZ);
	}
	void CommandBufferD3D12::BindBuffer(RGLBufferPtr buffer, uint32_t bindingOffset, uint32_t offsetIntoBuffer)
	{
		auto casted = std::static_pointer_cast<BufferD3D12>(buffer);
		const auto layout = currentRenderPipeline->pipelineLayout;
		const auto bindPoint = layout->slotForBufferIdx(bindingOffset);

		bool isUAV = layout->bufferIdxIsUAV(bindingOffset);

		SyncIfNeeded(static_cast<const BufferD3D12*>(buffer.get()), isUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_GENERIC_READ, isUAV);

		if (isUAV) {
			commandList->SetGraphicsRootUnorderedAccessView(bindPoint, casted->vertexBufferView.BufferLocation + offsetIntoBuffer);
		}
		else {
			commandList->SetGraphicsRootShaderResourceView(bindPoint, casted->vertexBufferView.BufferLocation + offsetIntoBuffer);
		}
	}
	void CommandBufferD3D12::BindComputeBuffer(RGLBufferPtr buffer, uint32_t bindingOffset, uint32_t offsetIntoBuffer)
	{

		auto casted = std::static_pointer_cast<BufferD3D12>(buffer);
		const auto currentLayout = currentComputePipeline->pipelineLayout;
		const auto slotidx = currentLayout->slotForBufferIdx(bindingOffset);
		 
		bool isUAV = currentLayout->bufferIdxIsUAV(bindingOffset);

		SyncIfNeeded(static_cast<const BufferD3D12*>(buffer.get()), isUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_GENERIC_READ, isUAV);

		if (isUAV) {
			commandList->SetComputeRootUnorderedAccessView(slotidx, casted->vertexBufferView.BufferLocation + offsetIntoBuffer);
		}
		else {
			commandList->SetComputeRootShaderResourceView(slotidx, casted->vertexBufferView.BufferLocation + offsetIntoBuffer);
		}

	}
	void CommandBufferD3D12::SetVertexBuffer(RGLBufferPtr buffer, const VertexBufferBinding& bindingInfo)
	{
		SyncIfNeeded(static_cast<const BufferD3D12*>(buffer.get()), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);

		commandList->IASetVertexBuffers(bindingInfo.bindingPosition, 1, &std::static_pointer_cast<BufferD3D12>(buffer)->vertexBufferView + bindingInfo.offsetIntoBuffer);
	}
	void CommandBufferD3D12::SetVertexBytes(const untyped_span data, uint32_t offset)
	{
		auto size = data.size() / 4 + (data.size() % 4 != 0 ? 1 : 0);
		commandList->SetGraphicsRoot32BitConstants(offset, size, data.data(), 0);
	}
	void CommandBufferD3D12::SetFragmentBytes(const untyped_span data, uint32_t offset)
	{
		// d3d does not make a distinction
		SetVertexBytes(data, offset);
	}
	void CommandBufferD3D12::SetComputeBytes(const untyped_span data, uint32_t offset)
	{
		auto size = data.size() / 4 + (data.size() % 4 != 0 ? 1 : 0);
		commandList->SetComputeRoot32BitConstants(offset, size, data.data(), 0);
	}
	void CommandBufferD3D12::SetIndexBuffer(RGLBufferPtr buffer)
	{
		SyncIfNeeded(static_cast<const BufferD3D12*>(buffer.get()), D3D12_RESOURCE_STATE_INDEX_BUFFER, false);
		commandList->IASetIndexBuffer(&(std::static_pointer_cast<BufferD3D12>(buffer)->indexBufferView));
	}
	void CommandBufferD3D12::SetVertexSampler(RGLSamplerPtr sampler, uint32_t index)
	{
		SetFragmentSampler(sampler, index);
	}
	void CommandBufferD3D12::SetFragmentSampler(RGLSamplerPtr sampler, uint32_t index)
	{
		auto thisSampler = std::static_pointer_cast<SamplerD3D12>(sampler);
		const auto pipelineLayout = currentRenderPipeline->pipelineLayout;
		const auto samplerSlot = pipelineLayout->slotForSamplerIdx(index);
		auto& samplerHeap = thisSampler->owningDevice->SamplerHeap;
		ID3D12DescriptorHeap* heapForThis[] = { samplerHeap->Heap() };
		commandList->SetDescriptorHeaps(std::size(heapForThis), heapForThis);
		commandList->SetGraphicsRootDescriptorTable(samplerSlot, samplerHeap->GetGpuHandle(thisSampler->descriptorIndex));
	}
	void CommandBufferD3D12::SetComputeSampler(RGLSamplerPtr sampler, uint32_t index)
	{
		auto thisSampler = std::static_pointer_cast<SamplerD3D12>(sampler);
		const auto pipelineLayout = currentComputePipeline->pipelineLayout;
		const auto samplerSlot = pipelineLayout->slotForSamplerIdx(index);
		auto& samplerHeap = thisSampler->owningDevice->SamplerHeap;
		ID3D12DescriptorHeap* heapForThis[] = { samplerHeap->Heap() };
		commandList->SetDescriptorHeaps(std::size(heapForThis), heapForThis);
		commandList->SetComputeRootDescriptorTable(samplerSlot, samplerHeap->GetGpuHandle(thisSampler->descriptorIndex));
	}
	void CommandBufferD3D12::SetVertexTexture(const TextureView& texture, uint32_t index)
	{
		SetFragmentTexture(texture, index);
	}
	void CommandBufferD3D12::SetFragmentTexture(const TextureView& texture, uint32_t index)
	{
		auto& thisTexture = texture.texture.dx;

		constexpr static auto depthReadState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		constexpr static auto colorReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		auto neededState = thisTexture.dsvAllocated() ? depthReadState : colorReadState;

		SyncIfNeeded(thisTexture, neededState);

		bool isGraphics = (bool)currentRenderPipeline;

		const auto pipelineLayout = isGraphics ? currentRenderPipeline->pipelineLayout : currentComputePipeline->pipelineLayout;
		const auto textureSlot = pipelineLayout->slotForTextureIdx(index);

		if (textureSlot.isUAV) {
			assert(thisTexture.uavAllocated(), "Cannot bind this texture because it is not in a UAV heap!");
		}
		else {
			assert(thisTexture.srvAllocated(), "Cannot bind this texture because it is not in a SRV heap!");
		}
		auto& heap = thisTexture.parentResource->owningDevice->CBV_SRV_UAVHeap;
		ID3D12DescriptorHeap* heapForThis[] = { heap->Heap() };
		commandList->SetDescriptorHeaps(std::size(heapForThis), heapForThis);
		if (isGraphics) {
			commandList->SetGraphicsRootDescriptorTable(textureSlot.slot, heap->GetGpuHandle(textureSlot.isUAV ? thisTexture.uavIDX : thisTexture.srvIDX));
		}
		else {
			commandList->SetComputeRootDescriptorTable(textureSlot.slot, heap->GetGpuHandle(textureSlot.isUAV ? thisTexture.uavIDX : thisTexture.srvIDX));
		}
	}
	void CommandBufferD3D12::SetComputeTexture(const TextureView& texture, uint32_t index)
	{
		SetFragmentTexture(texture, index);
	}
	void CommandBufferD3D12::Draw(uint32_t nVertices, const DrawInstancedConfig& config)
	{
		commandList->DrawInstanced(nVertices, config.nInstances, config.startVertex, config.firstInstance);
	}
	void CommandBufferD3D12::DrawIndexed(uint32_t nIndices, const DrawIndexedInstancedConfig& config)
	{
		commandList->DrawIndexedInstanced(nIndices, config.nInstances, config.firstIndex, config.startVertex, config.firstInstance);
	}
	void CommandBufferD3D12::SetViewport(const Viewport& viewport)
	{
		D3D12_VIEWPORT m_Viewport{
			.TopLeftX = viewport.x,
			.TopLeftY = viewport.y,
			.Width = viewport.width,
			.Height = viewport.height,
			.MinDepth = viewport.minDepth,
			.MaxDepth = viewport.maxDepth,
		};

		commandList->RSSetViewports(1, &m_Viewport);
	}
	void CommandBufferD3D12::SetScissor(const Rect& scissor)
	{
		D3D12_RECT m_ScissorRect{ CD3DX12_RECT{
			scissor.offset[0], 
			scissor.offset[1], 
			static_cast<LONG>(scissor.offset[0] + scissor.extent[0]), 
			static_cast<LONG>(scissor.offset[1] + scissor.extent[1])
		}};

		commandList->RSSetScissorRects(1, &m_ScissorRect);
	}

	void CommandBufferD3D12::CopyTextureToBuffer(TextureView& sourceTexture, const Rect& sourceRect, size_t offset, RGLBufferPtr desetBuffer)
	{
		auto casted = sourceTexture.texture.dx.parentResource;
		auto castedDest = std::static_pointer_cast<BufferD3D12>(desetBuffer);
		D3D12_TEXTURE_COPY_LOCATION destination{
			.pResource = castedDest->buffer.Get(),
			.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
		};

		D3D12_RESOURCE_DESC srcDesc = casted->texture->GetDesc();
		castedDest->owningDevice->device->GetCopyableFootprints(
			&srcDesc, 0, 1, 0,
			&destination.PlacedFootprint,
			NULL, NULL, NULL
		);

		// set bounds
		destination.PlacedFootprint.Footprint.Width = sourceRect.extent[0];
		destination.PlacedFootprint.Footprint.Height = sourceRect.extent[1];

		D3D12_TEXTURE_COPY_LOCATION source{
			.pResource = casted->texture.Get(),
			.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
			.SubresourceIndex = 0,
		};

		D3D12_BOX srcBox{
			.left = static_cast<UINT>(sourceRect.offset[0]),
			.top = static_cast<UINT>(sourceRect.offset[1]),
			.front = 0,
			.right = static_cast<UINT>(sourceRect.offset[0] + sourceRect.extent[0]),
			.bottom = static_cast<UINT>(sourceRect.offset[1] + sourceRect.extent[1]),
			.back = 1,
		};

		commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &srcBox);
	}
	void CommandBufferD3D12::CopyBufferToBuffer(BufferCopyConfig from, BufferCopyConfig to, uint32_t size)
	{
		auto fromBuffer = std::static_pointer_cast<BufferD3D12>(from.buffer);
		auto toBuffer = std::static_pointer_cast<BufferD3D12>(to.buffer);

		auto fromBufferCurrentState = GetBufferCurrentResourceState(fromBuffer.get());
		SyncIfNeeded(fromBuffer.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, true);

		auto oldState = GetBufferCurrentResourceState(toBuffer.get());

		auto preBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			toBuffer->buffer.Get(),
			oldState,
			D3D12_RESOURCE_STATE_COPY_DEST 
		);
		commandList->ResourceBarrier(1, &preBarrier);
		commandList->CopyBufferRegion(toBuffer->buffer.Get(), to.offset, fromBuffer->buffer.Get(), from.offset, size);

		auto postBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
			toBuffer->buffer.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			oldState
		);
		commandList->ResourceBarrier(1, &postBarrier);
		SyncIfNeeded(fromBuffer.get(), fromBufferCurrentState, true);
	}

	void CommandBufferD3D12::CopyTextureToTexture(const TextureCopyConfig& from, const TextureCopyConfig& to)
	{
		
		// Create a source texture location
		D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
		srcLocation.pResource = from.texture.texture.dx.parentResource->texture.Get();
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLocation.SubresourceIndex = 0;

		// Create a destination texture location
		D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
		dstLocation.pResource = to.texture.texture.dx.parentResource->texture.Get();
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = 0;

		// Create a box that specifies the region to copy
		D3D12_BOX box = {};
		box.left = 0;
		box.top = 0;
		box.front = 0;
		box.right = srcLocation.pResource->GetDesc().Width;
		box.bottom = srcLocation.pResource->GetDesc().Height;
		box.back = 1;

		// Copy the region from the source texture to the destination texture
		SyncIfNeeded(from.texture.texture.dx, D3D12_RESOURCE_STATE_COPY_SOURCE, true);
		SyncIfNeeded(to.texture.texture.dx, D3D12_RESOURCE_STATE_COPY_DEST, true);
		commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &box);

	}

	void CommandBufferD3D12::Commit(const CommitConfig& config)
	{
		owningQueue->ExecuteCommandList(commandList);
		if (config.signalFence) {
			auto d3d12fence = std::static_pointer_cast<FenceD3D12>(config.signalFence);
			owningQueue->m_d3d12CommandQueue->Signal(d3d12fence->fence.Get(), 1);	// 1 because we emulate binary vulkan fences
		}
		owningQueue->m_d3d12CommandQueue->Signal(internalFence.Get(), 1);	// for blockuntilcompleted
	}
	void CommandBufferD3D12::ExecuteIndirectIndexed(const IndirectConfig& config)
	{
		auto buffer = std::static_pointer_cast<BufferD3D12>(config.indirectBuffer);

		SyncIfNeeded(static_cast<const BufferD3D12*>(buffer.get()), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, false);

		auto sig = buffer->owningDevice->multidrawIndexedSignature;
		commandList->ExecuteIndirect(
			sig.Get(),
			config.nDraws,
			buffer->buffer.Get(),
			config.offsetIntoBuffer,
			nullptr,
			0
		);
	}
	void CommandBufferD3D12::ExecuteIndirect(const IndirectConfig& config)
	{
		auto buffer = std::static_pointer_cast<BufferD3D12>(config.indirectBuffer);

		SyncIfNeeded(static_cast<const BufferD3D12*>(buffer.get()), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, false);

		auto sig = buffer->owningDevice->multidrawSignature;
		commandList->ExecuteIndirect(
			sig.Get(),
			config.nDraws,
			buffer->buffer.Get(),
			config.offsetIntoBuffer,
			nullptr,
			0
		);
	}
	void CommandBufferD3D12::BeginRenderDebugMarker(const std::string& label)
	{
#ifdef PIX_SUPPORTED
		PIXBeginEvent(commandList.Get(), 0, label.c_str());
#endif
	}
	void CommandBufferD3D12::BeginComputeDebugMarker(const std::string& label)
	{
		BeginRenderDebugMarker(label);
	}
	void CommandBufferD3D12::EndRenderDebugMarker()
	{
#ifdef PIX_SUPPORTED
		PIXEndEvent(commandList.Get());
#endif
	}
	void CommandBufferD3D12::EndComputeDebugMarker()
	{
		EndRenderDebugMarker();
	}
	void CommandBufferD3D12::BlockUntilCompleted()
	{
		if (internalFence->GetCompletedValue() != 1) {
			internalFence->SetEventOnCompletion(1, internalFenceEvent);

			::WaitForSingleObject(internalFenceEvent, DWORD_MAX);
		}
	}
	CommandBufferD3D12::~CommandBufferD3D12()
	{
		CloseHandle(internalFenceEvent);
	}
	void CommandBufferD3D12::SyncIfNeeded(const BufferD3D12* buffer, D3D12_RESOURCE_STATES needed, bool written)
	{
		// only UAV buffers need to be transitioned
		if (!buffer->isWritable && !buffer->myType.StorageBuffer && !buffer->myType.IndirectBuffer) {
			return;
		}

		// track the resource if it's not already in there
		auto it = activeBuffers.find(buffer);

		if (it == activeBuffers.end()) {
			activeBuffers[buffer] = {
				.state = buffer->nativeState,
				.written = written
			};
			it = activeBuffers.find(buffer);
		}

		auto current = (*it).second.state;

		CD3DX12_RESOURCE_BARRIER barriers[2];
		uint32_t nBarriers = 0;

		// a resource transition is in order
		if (buffer->canBeTransitioned && current != needed) {

			// is the transition possible?
			if (needed == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && !buffer->isWritable) {
				goto barrierBail;
			}

			barriers[nBarriers] = CD3DX12_RESOURCE_BARRIER::Transition(
				buffer->buffer.Get(),
				current,
				needed
			);
			// update tracker
			(*it).second = {
				.state = needed,
				.written = written
			};

			nBarriers++;
		}
	barrierBail:

		if ((*it).second.written && buffer->isWritable) {
			// do a simple UAV barrier because access needs to be synchronized here
			barriers[nBarriers] = CD3DX12_RESOURCE_BARRIER::UAV(buffer->buffer.Get());
			nBarriers++;
		}

		//TODO: barriers of size 1 are inefficient. We should batch these somehow.
		if (nBarriers > 0) {
			commandList->ResourceBarrier(nBarriers, barriers);
		}
		// update written state
		it->second.written = written;
		
	}
	void CommandBufferD3D12::SyncIfNeeded(TextureView texture, D3D12_RESOURCE_STATES needed, bool written)
	{
		D3D12TextureLastUseKey key{texture.texture.dx.parentResource, texture.texture.dx.mip};

		if (keyIsAllMips(key)) {
			// transition all the mips
			// must be done individually because they could all be in different states
			auto numMips = texture.texture.dx.parentResource->numMips;
			for (uint32_t i = 0; i < numMips; i++) {
				TextureView copy = texture;
				copy.texture.dx.mip = i;
				SyncIfNeeded(copy, needed);
			}
		}
		else {
			auto it = activeTextures.find(key);

			if (it == activeTextures.end()) {
				activeTextures[key] = {
					.state = texture.texture.dx.parentResource->nativeState,
					.written = false
				};
				it = activeTextures.find(key);
			}

			auto current = (*it).second.state;
			if (current == needed) {
				return;
			}

			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.texture.dx.parentResource->GetResource(),
				current,
				needed,
				texture.texture.dx.mip
			);
			// update tracker
			(*it).second = {
				.state = needed,
				.written = written
			};

			//TODO: barriers of size 1 are inefficient. We should batch these somehow.
			commandList->ResourceBarrier(1, &barrier);
		}
	}
	D3D12_RESOURCE_STATES CommandBufferD3D12::GetBufferCurrentResourceState(const D3D12TrackedResource* resource)
	{
		auto it = activeBuffers.find(resource);
		if (it != activeBuffers.end()) {
			return it->second.state;
		}
		else {
			return resource->nativeState;
		}
	}
}

#endif
