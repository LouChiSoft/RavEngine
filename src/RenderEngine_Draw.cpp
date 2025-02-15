#if !RVE_SERVER
#define GLM_FORCE_DEPTH_ZERO_TO_ONE 1
#include "RenderEngine.hpp"
#include <RGL/CommandBuffer.hpp>
#include <RGL/Swapchain.hpp>
#include <RGL/RenderPass.hpp>
#include "World.hpp"
#include "Skybox.hpp"
#include "DebugDraw.h"
#include "DebugDrawer.hpp"
#include <im3d.h>
#include <GUI.hpp>
#include <AnimatorComponent.hpp>
#include "MeshAssetSkinned.hpp"
#include "MeshAsset.hpp"
#include "SkeletonAsset.hpp"
#include "Texture.hpp"
#include "Debug.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <chrono>
#include <numbers>
#include "Transform.hpp"


namespace RavEngine {

struct LightingType{
    bool Lit: 1 = false;
    bool Unlit: 1 = false;
};

#ifndef NDEBUG
	static DebugDrawer dbgdraw;	//for rendering debug primitives
#endif

	/**
 Render one frame using the current state of every object in the world
 */
	RGLCommandBufferPtr RenderEngine::Draw(Ref<RavEngine::World> worldOwning, const std::vector<RenderViewCollection>& targets, float guiScaleFactor) {
		auto start = std::chrono::high_resolution_clock::now();
		transientOffset = 0;

		
		DestroyUnusedResources();
		mainCommandBuffer->Reset();
		mainCommandBuffer->Begin();
		
		auto worldTransformBuffer = worldOwning->renderData->worldTransforms.buffer;

		// do skeletal operations
		struct skeletalMeshPrepareResult {
			bool skeletalMeshesExist = false;
		};
		auto prepareSkeletalMeshBuffers = [this, &worldOwning,&worldTransformBuffer]() -> skeletalMeshPrepareResult {
			// count objects
			uint32_t totalVertsToSkin = 0;
			uint32_t totalJointsToSkin = 0;
			uint32_t totalObjectsToSkin = 0;	// also the number of draw calls in the indirect buffer


			// resize buffers if they need to be resized
			auto resizeSkeletonBuffer = [this](RGLBufferPtr& buffer, uint32_t stride, uint32_t neededSize, RGL::BufferConfig::Type type, RGL::BufferAccess access, RGL::BufferFlags options = {}) {
				uint32_t currentCount = 0;
				if (!buffer || buffer->getBufferSize() / stride < neededSize) {
					if (buffer) {
						currentCount = buffer->getBufferSize() / stride;
						gcBuffers.enqueue(buffer);
					}
					auto newSize = closest_power_of(neededSize, 2);
					if (newSize == 0) {
						return;
					}
					buffer = device->CreateBuffer({
						newSize,
						type,
						stride,
						access,
						options
						});
					if (access == RGL::BufferAccess::Shared) {
						buffer->MapMemory();
					}
				}
			};

			for (auto& [materialInstance, drawcommand] : worldOwning->renderData->skinnedMeshRenderData) {
				uint32_t totalEntitiesForThisCommand = 0;
				for (auto& command : drawcommand.commands) {
					auto subCommandEntityCount = command.entities.DenseSize();
					totalObjectsToSkin += subCommandEntityCount;
					totalEntitiesForThisCommand += subCommandEntityCount;

					if (auto mesh = command.mesh.lock()) {
						totalVertsToSkin += mesh->GetNumVerts();
					}

					if (auto skeleton = command.skeleton.lock()) {
						totalJointsToSkin += skeleton->GetSkeleton()->num_joints();
					}
				}

				resizeSkeletonBuffer(drawcommand.indirectBuffer, sizeof(RGL::IndirectIndexedCommand), totalEntitiesForThisCommand + 5, { .StorageBuffer = true, .IndirectBuffer = true }, RGL::BufferAccess::Private, { .Writable = true, .debugName = "Skeleton per-material IndirectBuffer" });
				//TODO: skinned meshes do not support LOD groups
				resizeSkeletonBuffer(drawcommand.cullingBuffer, sizeof(entity_t), totalEntitiesForThisCommand + 5, { .StorageBuffer = true, .VertexBuffer = true }, RGL::BufferAccess::Private, { .Writable = true, .debugName = "Skeleton per-material culingBuffer" });
			}

			resizeSkeletonBuffer(sharedSkeletonMatrixBuffer, sizeof(matrix4), totalJointsToSkin, { .StorageBuffer = true }, RGL::BufferAccess::Shared, { .debugName = "sharedSkeletonMatrixBuffer" });
			resizeSkeletonBuffer(sharedSkinnedMeshVertexBuffer, sizeof(VertexNormalUV), totalVertsToSkin, { .StorageBuffer = true, .VertexBuffer = true }, RGL::BufferAccess::Private, { .Writable = true, .debugName = "sharedSkinnedMeshVertexBuffer" });

			return {
				.skeletalMeshesExist = totalObjectsToSkin > 0 && totalVertsToSkin > 0
			};
		};
		auto skeletalPrepareResult = prepareSkeletalMeshBuffers();

		auto prepareSkeletalCullingBuffer = [this,&worldOwning]() {
			// dispatch compute to build the indirect buffer for finally rendering the skinned meshes
			// each skinned mesh gets its own 1-instance draw in the buffer. The instance count starts at 0.
			mainCommandBuffer->BeginComputeDebugMarker("Prepare Skinned Indirect Draw buffer");
			mainCommandBuffer->BeginCompute(skinningDrawCallPreparePipeline);
			SkinningPrepareUBO ubo;
			uint32_t baseInstance = 0;
			for (auto& [materialInstance, drawcommand] : worldOwning->renderData->skinnedMeshRenderData) {
				mainCommandBuffer->BindComputeBuffer(drawcommand.indirectBuffer, 0, 0);
				for (auto& command : drawcommand.commands) {
					const auto objectCount = command.entities.DenseSize();
					const auto mesh = command.mesh.lock();
					const auto vertexCount = mesh->GetNumVerts();

					ubo.nVerticesInThisMesh = vertexCount;
					ubo.nTotalObjects = objectCount;
					ubo.indexBufferOffset = mesh->meshAllocation.indexRange->start / sizeof(uint32_t);
					ubo.nIndicesInThisMesh = mesh->GetNumIndices();

					mainCommandBuffer->SetComputeBytes(ubo, 0);
					mainCommandBuffer->DispatchCompute(std::ceil(objectCount / 32.0f), 1, 1, 32, 1, 1);

					ubo.vertexBufferOffset += vertexCount;
					ubo.drawCallBufferOffset += objectCount;
					ubo.baseInstanceOffset += objectCount;
				}
			}
			mainCommandBuffer->EndCompute();
			mainCommandBuffer->EndComputeDebugMarker();
		};
		

		auto poseSkeletalMeshes = [this,&worldOwning]() {
			mainCommandBuffer->BeginComputeDebugMarker("Pose Skinned Meshes");
			mainCommandBuffer->BeginCompute(skinnedMeshComputePipeline);
			mainCommandBuffer->BindComputeBuffer(sharedSkinnedMeshVertexBuffer, 0);
			mainCommandBuffer->BindComputeBuffer(sharedVertexBuffer, 1);
			mainCommandBuffer->BindComputeBuffer(sharedSkeletonMatrixBuffer, 2);
			using mat_t = glm::mat4;
			std::span<mat_t> matbufMem{ static_cast<mat_t*>(sharedSkeletonMatrixBuffer->GetMappedDataPtr()), sharedSkeletonMatrixBuffer->getBufferSize() / sizeof(mat_t) };
			SkinningUBO subo;
			for (auto& [materialInstance, drawcommand] : worldOwning->renderData->skinnedMeshRenderData) {
				for (auto& command : drawcommand.commands) {
					auto skeleton = command.skeleton.lock();
					auto mesh = command.mesh.lock();
					auto& entities = command.entities;
					mainCommandBuffer->BindComputeBuffer(mesh->GetWeightsBuffer(), 3);

					subo.numObjects = command.entities.DenseSize();
					subo.numVertices = mesh->GetNumVerts();
					subo.numBones = skeleton->GetSkeleton()->num_joints();
					subo.vertexReadOffset = mesh->meshAllocation.vertRange->start / sizeof(VertexNormalUV);

					// write joint transform matrices into buffer and update uniform offset
					for (const auto& ownerid : command.entities.reverse_map) {
						auto& animator = worldOwning->GetComponent<AnimatorComponent>(ownerid);
						const auto& skinningMats = animator.GetSkinningMats();
						std::copy(skinningMats.begin(), skinningMats.end(), matbufMem.begin() + subo.boneReadOffset);
					}

					mainCommandBuffer->SetComputeBytes(subo, 0);
					mainCommandBuffer->DispatchCompute(std::ceil(subo.numObjects / 8.0f), std::ceil(subo.numVertices / 32.0f), 1, 8, 32, 1);
					subo.boneReadOffset += subo.numBones * subo.numObjects;
					subo.vertexWriteOffset += subo.numVertices * subo.numObjects;	// one copy of the vertex data per object
				}
			}
			mainCommandBuffer->EndCompute();
			mainCommandBuffer->EndComputeDebugMarker();
		};
		
		// don't do operations if there's nothing to skin
		// these operations run once per frame since the results
		// are the same for all future passes
		if (skeletalPrepareResult.skeletalMeshesExist) {
			poseSkeletalMeshes();

			prepareSkeletalCullingBuffer();
		}

		for (const auto& view : targets) {
			currentRenderSize = view.pixelDimensions;
			auto nextImgSize = view.pixelDimensions;
			auto& target = view.collection;

			auto cullSkeletalMeshes = [this, &worldTransformBuffer, &worldOwning, &target](matrix4 viewproj) {
				// the culling shader will decide for each draw if the draw should exist (and set its instance count to 1 from 0).

				mainCommandBuffer->BeginComputeDebugMarker("Cull Skinned Meshes");
				mainCommandBuffer->BeginCompute(defaultCullingComputePipeline);
				mainCommandBuffer->BindComputeBuffer(worldTransformBuffer, 1);
				for (auto& [materialInstance, drawcommand] : worldOwning->renderData->skinnedMeshRenderData) {
					CullingUBO cubo{
						.viewProj = viewproj,
						.indirectBufferOffset = 0,
					};
					for (auto& command : drawcommand.commands) {
						mainCommandBuffer->BindComputeBuffer(drawcommand.cullingBuffer, 2);
						mainCommandBuffer->BindComputeBuffer(drawcommand.indirectBuffer, 3);

						if (auto mesh = command.mesh.lock()) {
							uint32_t lodsForThisMesh = 1;	// TODO: skinned meshes do not support LOD groups 

							cubo.numObjects = command.entities.DenseSize();
							mainCommandBuffer->BindComputeBuffer(command.entities.GetDense().get_underlying().buffer, 0);
							cubo.radius = mesh->radius;
#if __APPLE__
							constexpr size_t byte_size = closest_multiple_of<ssize_t>(sizeof(cubo), 16);
							std::byte bytes[byte_size]{};
							std::memcpy(bytes, &cubo, sizeof(cubo));
							mainCommandBuffer->SetComputeBytes({ bytes, sizeof(bytes) }, 0);
#else
							mainCommandBuffer->SetComputeBytes(cubo, 0);
#endif
							mainCommandBuffer->SetComputeTexture(target.depthPyramidTexture->GetDefaultView(), 4);
							mainCommandBuffer->SetComputeSampler(depthPyramidSampler, 5);
							mainCommandBuffer->DispatchCompute(std::ceil(cubo.numObjects / 64.f), 1, 1, 64, 1, 1);
							cubo.indirectBufferOffset += lodsForThisMesh;
							cubo.cullingBufferOffset += lodsForThisMesh * command.entities.DenseSize();
						}
					}

				}
				mainCommandBuffer->EndComputeDebugMarker();
				mainCommandBuffer->EndCompute();
			};


			auto renderFromPerspective = [this, &worldTransformBuffer, &worldOwning, &skeletalPrepareResult, &cullSkeletalMeshes, &target](matrix4 viewproj, vector3 camPos, RGLRenderPassPtr renderPass, auto&& pipelineSelectorFunction, RGL::Rect viewportScissor, LightingType lightingFilter) {

				auto cullTheRenderData = [this, &viewproj, &worldTransformBuffer, &camPos,&target](auto&& renderData) {
					for (auto& [materialInstance, drawcommand] : renderData) {
						//prepass: get number of LODs and entities
						uint32_t numLODs = 0, numEntities = 0;
						for (const auto& command : drawcommand.commands) {
							if (auto mesh = command.mesh.lock()) {
								numLODs += mesh->GetNumLods();
								numEntities += command.entities.DenseSize();
							}
						}

						auto reallocBuffer = [this](RGLBufferPtr& buffer, uint32_t size_count, uint32_t stride, RGL::BufferAccess access, RGL::BufferConfig::Type type, RGL::BufferFlags flags) {
							if (buffer == nullptr || buffer->getBufferSize() < size_count * stride) {
								// trash old buffer if it exists
								if (buffer) {
									gcBuffers.enqueue(buffer);
								}
								buffer = device->CreateBuffer({
									size_count,
									type,
									stride,
									access,
									flags
									});
								if (access == RGL::BufferAccess::Shared) {
									buffer->MapMemory();
								}
							}
						};
						const auto cullingbufferTotalSlots = numEntities * numLODs;
						reallocBuffer(drawcommand.cullingBuffer, cullingbufferTotalSlots, sizeof(entity_t), RGL::BufferAccess::Private, { .StorageBuffer = true, .VertexBuffer = true }, { .Writable = true, .debugName = "Culling Buffer" });
						reallocBuffer(drawcommand.indirectBuffer, numLODs, sizeof(RGL::IndirectIndexedCommand), RGL::BufferAccess::Private, { .StorageBuffer = true, .IndirectBuffer = true }, { .Writable = true, .debugName = "Indirect Buffer" });
						reallocBuffer(drawcommand.indirectStagingBuffer, numLODs, sizeof(RGL::IndirectIndexedCommand), RGL::BufferAccess::Shared, { .StorageBuffer = true }, { .Transfersource = true, .Writable = false,.debugName = "Indirect Staging Buffer" });

						// initial populate of drawcall buffer
						// we need one command per mesh per LOD
						{
							uint32_t meshID = 0;
							uint32_t baseInstance = 0;
							for (const auto& command : drawcommand.commands) {			// for each mesh
								const auto nEntitiesInThisCommand = command.entities.DenseSize();
								RGL::IndirectIndexedCommand initData;
								if (auto mesh = command.mesh.lock()) {
									for (uint32_t lodID = 0; lodID < mesh->GetNumLods(); lodID++) {
										initData = {
											.indexCount = uint32_t(mesh->totalIndices),
											.instanceCount = 0,
											.indexStart = uint32_t(mesh->meshAllocation.indexRange->start / sizeof(uint32_t)),
											.baseVertex = uint32_t(mesh->meshAllocation.vertRange->start / sizeof(VertexNormalUV)),
											.baseInstance = baseInstance,	// sets the offset into the material-global culling buffer (and other per-instance data buffers). we allocate based on worst-case here, so the offset is known.
										};
										baseInstance += nEntitiesInThisCommand;
										drawcommand.indirectStagingBuffer->UpdateBufferData(initData, (meshID + lodID) * sizeof(RGL::IndirectIndexedCommand));
									}

								}
								meshID++;
							}
						}
						mainCommandBuffer->CopyBufferToBuffer(
							{
								.buffer = drawcommand.indirectStagingBuffer,
								.offset = 0
							},
				{
					.buffer = drawcommand.indirectBuffer,
					.offset = 0
				}, drawcommand.indirectStagingBuffer->getBufferSize());

						mainCommandBuffer->BeginCompute(defaultCullingComputePipeline);
						mainCommandBuffer->BindComputeBuffer(worldTransformBuffer, 1);
						CullingUBO cubo{
							.viewProj = viewproj,
							.camPos = camPos,
							.indirectBufferOffset = 0,
						};
						static_assert(sizeof(cubo) <= 128, "CUBO is too big!");
						for (auto& command : drawcommand.commands) {
							mainCommandBuffer->BindComputeBuffer(drawcommand.cullingBuffer, 2);
							mainCommandBuffer->BindComputeBuffer(drawcommand.indirectBuffer, 3);

							if (auto mesh = command.mesh.lock()) {
								uint32_t lodsForThisMesh = mesh->GetNumLods();

								cubo.numObjects = command.entities.DenseSize();
								mainCommandBuffer->BindComputeBuffer(command.entities.GetDense().get_underlying().buffer, 0);
                                cubo.radius = mesh->radius;
                                
#if __APPLE__
                                constexpr size_t byte_size = closest_multiple_of<ssize_t>(sizeof(cubo), 16);
                                std::byte bytes[byte_size]{};
                                std::memcpy(bytes, &cubo, sizeof(cubo));
                                mainCommandBuffer->SetComputeBytes({bytes, sizeof(bytes)}, 0);
#else
                                mainCommandBuffer->SetComputeBytes(cubo, 0);
#endif
								mainCommandBuffer->SetComputeTexture(target.depthPyramidTexture->GetDefaultView(), 4);
								mainCommandBuffer->SetComputeSampler(depthPyramidSampler, 5);
								mainCommandBuffer->DispatchCompute(std::ceil(cubo.numObjects / 64.f), 1, 1, 64, 1, 1);
								cubo.indirectBufferOffset += lodsForThisMesh;
								cubo.cullingBufferOffset += lodsForThisMesh * command.entities.DenseSize();
							}
						}
						mainCommandBuffer->EndCompute();
					}
				};
				auto renderTheRenderData = [this, &viewproj, &worldTransformBuffer, &pipelineSelectorFunction, &viewportScissor](auto&& renderData, RGLBufferPtr vertexBuffer, LightingType currentLightingType) {
					// do static meshes
					mainCommandBuffer->SetViewport({
						.x = float(viewportScissor.offset[0]),
						.y = float(viewportScissor.offset[1]),
						.width = static_cast<float>(viewportScissor.extent[0]),
						.height = static_cast<float>(viewportScissor.extent[1]),
						});
					mainCommandBuffer->SetScissor(viewportScissor);
					mainCommandBuffer->SetVertexBuffer(vertexBuffer);
					mainCommandBuffer->SetIndexBuffer(sharedIndexBuffer);
					for (auto& [materialInstanceVariant, drawcommand] : renderData) {
                        
                        // get the material instance out
                        Ref<MaterialInstance> materialInstance;
                        std::visit([&materialInstance, currentLightingType] (const auto& var) {
                            if constexpr (std::is_same_v<std::decay_t<decltype(var)>,LitMeshMaterialInstance>){
                                if (currentLightingType.Lit){
                                    materialInstance = var.material;
                                }
                            }
                            else if constexpr(std::is_same_v<std::decay_t<decltype(var)>,UnlitMeshMaterialInstance>){
                                if (currentLightingType.Unlit){
                                    materialInstance = var.material;
                                }
                            }
                            // materialInstance will be unset (== nullptr) if the match is invalid
                        },materialInstanceVariant);
                        
                        // is this the correct material type? if not, skip
                        if (materialInstance == nullptr){
                            continue;
                        }
                        
						// bind the pipeline
						auto pipeline = pipelineSelectorFunction(materialInstance->GetMat());
						mainCommandBuffer->BindRenderPipeline(pipeline);
                        
                        
                        

						// set push constant data
						auto pushConstantData = materialInstance->GetPushConstantData();

                        // Metal requires 16-byte alignment, so we bake that into the required size
						size_t pushConstantTotalSize =
#if __APPLE__
							closest_multiple_of<ssize_t>(sizeof(viewproj) + pushConstantData.size(), 16);
#else
							sizeof(viewproj) + pushConstantData.size();
#endif
                        
                        // AMD on vulkan cannot accept push constants > 128 bytes so we cap it there for all platforms
                        std::byte totalPushConstantBytes[128]{};
                        Debug::Assert(pushConstantTotalSize < std::size(totalPushConstantBytes), "Cannot write push constants, total size ({}) > {}", pushConstantTotalSize, std::size(totalPushConstantBytes));

						std::memcpy(totalPushConstantBytes, &viewproj, sizeof(viewproj));
						if (pushConstantData.size() > 0 && pushConstantData.data() != nullptr) {
							std::memcpy(totalPushConstantBytes + sizeof(viewproj), pushConstantData.data(), pushConstantData.size());
						}

						mainCommandBuffer->SetVertexBytes({ totalPushConstantBytes ,pushConstantTotalSize }, 0);
						mainCommandBuffer->SetFragmentBytes({ totalPushConstantBytes ,pushConstantTotalSize }, 0);

						// bind textures and buffers
						auto& bufferBindings = materialInstance->GetBufferBindings();
						auto& textureBindings = materialInstance->GetTextureBindings();
						for (int i = 0; i < materialInstance->maxBindingSlots; i++) {
							auto& buffer = bufferBindings[i];
							auto& texture = textureBindings[i];
							if (buffer) {
								mainCommandBuffer->BindBuffer(buffer, i);
							}
							if (texture) {
								mainCommandBuffer->SetFragmentSampler(textureSampler, 0); // TODO: don't hardcode this
								mainCommandBuffer->SetFragmentTexture(texture->GetRHITexturePointer()->GetDefaultView(), i);
							}
						}

						// bind the culling buffer and the transform buffer
						mainCommandBuffer->SetVertexBuffer(drawcommand.cullingBuffer, { .bindingPosition = 1 });
						mainCommandBuffer->BindBuffer(worldTransformBuffer, 10);

						// do the indirect command
						mainCommandBuffer->ExecuteIndirectIndexed({
							.indirectBuffer = drawcommand.indirectBuffer,
							.nDraws = uint32_t(drawcommand.indirectBuffer->getBufferSize() / sizeof(RGL::IndirectIndexedCommand))	// the number of structs in the buffer
							});
					}
				};

				// do culling operations
				mainCommandBuffer->BeginComputeDebugMarker("Cull Static Meshes");
				cullTheRenderData(worldOwning->renderData->staticMeshRenderData);
				mainCommandBuffer->EndComputeDebugMarker();
				if (skeletalPrepareResult.skeletalMeshesExist) {
					cullSkeletalMeshes(viewproj);
				}


				// do rendering operations
				mainCommandBuffer->BeginRendering(renderPass);
				mainCommandBuffer->BeginRenderDebugMarker("Render Static Meshes");
                renderTheRenderData(worldOwning->renderData->staticMeshRenderData, sharedVertexBuffer, lightingFilter);
				mainCommandBuffer->EndRenderDebugMarker();
				if (skeletalPrepareResult.skeletalMeshesExist) {
					mainCommandBuffer->BeginRenderDebugMarker("Render Skinned Meshes");
                    renderTheRenderData(worldOwning->renderData->skinnedMeshRenderData, sharedSkinnedMeshVertexBuffer, lightingFilter);
					mainCommandBuffer->EndRenderDebugMarker();
				}
				mainCommandBuffer->EndRendering();
			};

			auto renderDeferredPass = [this,&target, &renderFromPerspective](auto&& viewproj, auto&& camPos, auto&& fullSizeViewport, auto&& fullSizeScissor, auto&& renderArea) {
				// render all the static meshes

				renderFromPerspective(viewproj, camPos, deferredRenderPass, [](Ref<Material>&& mat) {
					return mat->GetMainRenderPipeline();
                }, renderArea, {.Lit = true});

				
			};

			auto renderLightingPass = [this, &target, &renderFromPerspective, &nextImgSize, &worldOwning](auto&& viewproj, auto&& camPos, auto&& fullSizeViewport, auto&& fullSizeScissor, auto&& renderArea) {
				// do lighting pass
				// these run in window coordinates, even if in split screen
				// but are confined by the scissor rect
				glm::ivec4 viewRect {0, 0, nextImgSize.width, nextImgSize.height};

				AmbientLightUBO ambientUBO{
					.viewRect = viewRect
				};

				auto invviewproj = glm::inverse(viewproj);

				// ambient lights
				if (worldOwning->renderData->ambientLightData.uploadData.DenseSize() > 0) {
					mainCommandBuffer->BeginRendering(ambientLightRenderPass);
					mainCommandBuffer->BeginRenderDebugMarker("Render Ambient Lights");
					mainCommandBuffer->BindRenderPipeline(ambientLightRenderPipeline);
					mainCommandBuffer->SetViewport(fullSizeViewport);
					mainCommandBuffer->SetScissor(fullSizeScissor);
					mainCommandBuffer->SetFragmentSampler(textureSampler, 0);
					mainCommandBuffer->SetFragmentTexture(target.diffuseTexture->GetDefaultView(), 1);

					mainCommandBuffer->SetVertexBuffer(screenTriVerts);
					mainCommandBuffer->SetVertexBytes(ambientUBO, 0);
					mainCommandBuffer->SetFragmentBytes(ambientUBO, 0);
					mainCommandBuffer->SetVertexBuffer(worldOwning->renderData->ambientLightData.uploadData.GetDense().get_underlying().buffer, {
						.bindingPosition = 1
						});
					mainCommandBuffer->Draw(3, {
						.nInstances = worldOwning->renderData->ambientLightData.uploadData.DenseSize()
						});
					mainCommandBuffer->EndRenderDebugMarker();
					mainCommandBuffer->EndRendering();
				}


				struct lightViewProjResult {
					glm::mat4 lightProj, lightView;
					glm::vec3 camPos = glm::vec3{ 0,0,0 };
				};

				auto renderLight = [this, &renderFromPerspective, &viewproj, &viewRect, target, &fullSizeScissor, &fullSizeViewport, &renderArea](auto&& lightStore, RGLRenderPipelinePtr lightPipeline, uint32_t dataBufferStride, auto&& bindpolygonBuffers, auto&& drawCall, auto&& genLightViewProj) {
					if (lightStore.uploadData.DenseSize() > 0) {
						LightingUBO lightUBO{
							.viewProj = viewproj,
							.viewRect = viewRect,
							.viewRegion = {renderArea.offset[0],renderArea.offset[1],renderArea.extent[0],renderArea.extent[1]}
						};

						shadowRenderPass->SetDepthAttachmentTexture(shadowTexture->GetDefaultView());
						lightUBO.isRenderingShadows = true;
						for (uint32_t i = 0; i < lightStore.uploadData.DenseSize(); i++) {
							const auto& light = lightStore.uploadData.GetDense()[i];
                            
                            using lightadt_t = std::remove_reference_t<decltype(lightStore)>;
                            
                            void* aux_data = nullptr;
                            if constexpr (lightadt_t::hasAuxData){
                                aux_data = &lightStore.auxData.GetDense()[i];
                            }
                            
							if (!light.castsShadows) {
								continue;
							}

							struct {
								glm::mat4 lightViewProj;
							} lightExtras;

							lightViewProjResult lightMats = genLightViewProj(light,aux_data);

							auto lightSpaceMatrix = lightMats.lightProj * lightMats.lightView;
							lightUBO.camPos = lightMats.camPos;

							renderFromPerspective(lightSpaceMatrix, lightMats.camPos, shadowRenderPass, [](Ref<Material>&& mat) {
								return mat->GetShadowRenderPipeline();
                            }, { 0, 0, shadowMapSize,shadowMapSize }, {.Lit = true, .Unlit = true});

							lightExtras.lightViewProj = lightSpaceMatrix;

							auto transientOffset = WriteTransient(lightExtras);

							mainCommandBuffer->BeginRendering(lightingRenderPass);
							//reset viewport and scissor
							mainCommandBuffer->SetViewport(fullSizeViewport);
							mainCommandBuffer->SetScissor(fullSizeScissor);
							mainCommandBuffer->BindRenderPipeline(lightPipeline);
							mainCommandBuffer->SetFragmentSampler(textureSampler, 0);
							mainCommandBuffer->SetFragmentSampler(shadowSampler, 1);

							mainCommandBuffer->SetFragmentTexture(target.diffuseTexture->GetDefaultView(), 2);
							mainCommandBuffer->SetFragmentTexture(target.normalTexture->GetDefaultView(), 3);
							mainCommandBuffer->SetFragmentTexture(target.depthStencil->GetDefaultView(), 4);
							mainCommandBuffer->SetFragmentTexture(shadowTexture->GetDefaultView(), 5);
							mainCommandBuffer->SetFragmentTexture(target.roughnessSpecularMetallicAOTexture->GetDefaultView(), 6);

							mainCommandBuffer->BindBuffer(transientBuffer, 8, transientOffset);

							bindpolygonBuffers(mainCommandBuffer);
							mainCommandBuffer->SetVertexBytes(lightUBO, 0);
							mainCommandBuffer->SetFragmentBytes(lightUBO, 0);
							mainCommandBuffer->SetVertexBuffer(lightStore.uploadData.GetDense().get_underlying().buffer, {
								.bindingPosition = 1,
								.offsetIntoBuffer = uint32_t(dataBufferStride * i)
								});
							drawCall(mainCommandBuffer, 1);
							mainCommandBuffer->EndRendering();
						}

						lightUBO.isRenderingShadows = false;
						mainCommandBuffer->BeginRendering(lightingRenderPass);
						mainCommandBuffer->BindRenderPipeline(lightPipeline);

						mainCommandBuffer->SetFragmentSampler(textureSampler, 0);
						mainCommandBuffer->SetFragmentSampler(shadowSampler, 1);

						mainCommandBuffer->SetFragmentTexture(target.diffuseTexture->GetDefaultView(), 2);
						mainCommandBuffer->SetFragmentTexture(target.normalTexture->GetDefaultView(), 3);
						mainCommandBuffer->SetFragmentTexture(target.depthStencil->GetDefaultView(), 4);
						mainCommandBuffer->SetFragmentTexture(target.roughnessSpecularMetallicAOTexture->GetDefaultView(), 6);
						mainCommandBuffer->SetFragmentTexture(shadowTexture->GetDefaultView(), 5);

						mainCommandBuffer->BindBuffer(transientBuffer, 8, transientOffset);

						bindpolygonBuffers(mainCommandBuffer);
						mainCommandBuffer->SetVertexBytes(lightUBO, 0);
						mainCommandBuffer->SetFragmentBytes(lightUBO, 0);
						mainCommandBuffer->SetVertexBuffer(lightStore.uploadData.GetDense().get_underlying().buffer, {
							.bindingPosition = 1
							});
						drawCall(mainCommandBuffer, lightStore.uploadData.DenseSize());
						mainCommandBuffer->EndRendering();
					}
				};

				// directional lights
				mainCommandBuffer->BeginRenderDebugMarker("Render Directional Lights");
				renderLight(worldOwning->renderData->directionalLightData, dirLightRenderPipeline, sizeof(World::DirLightUploadData),
					[this](RGLCommandBufferPtr mainCommandBuffer) {
						mainCommandBuffer->SetVertexBuffer(screenTriVerts);
					},
					[](RGLCommandBufferPtr mainCommandBuffer, uint32_t nInstances) {
						mainCommandBuffer->Draw(3, {
							.nInstances = nInstances
							});
					},
					[&camPos](const RavEngine::World::DirLightUploadData& light, auto auxDataPtr) {
						auto dirvec = light.direction;
                    
                        auto auxdata = static_cast<World::DirLightAuxData*>(auxDataPtr);

                        auto lightArea = auxdata->shadowDistance;

						auto lightProj = RMath::orthoProjection<float>(-lightArea, lightArea, -lightArea, lightArea, -100, 100);
						auto lightView = glm::lookAt(dirvec, { 0,0,0 }, { 0,1,0 });
						const vector3 reposVec{ std::round(-camPos.x), std::round(camPos.y), std::round(-camPos.z) };
						lightView = glm::translate(lightView, reposVec);

						return lightViewProjResult{
							.lightProj = lightProj,
							.lightView = lightView,
							.camPos = camPos
						};
					}
				);
				mainCommandBuffer->EndRenderDebugMarker();

				// spot lights
				mainCommandBuffer->BeginRenderDebugMarker("Render Spot Lights");
				renderLight(worldOwning->renderData->spotLightData, spotLightRenderPipeline, sizeof(World::SpotLightDataUpload),
					[this](RGLCommandBufferPtr mainCommandBuffer) {
						mainCommandBuffer->SetVertexBuffer(spotLightVertexBuffer);
						mainCommandBuffer->SetIndexBuffer(spotLightIndexBuffer);
					},
					[this](RGLCommandBufferPtr mainCommandBuffer, uint32_t nInstances) {
						mainCommandBuffer->DrawIndexed(nSpotLightIndices, {
							.nInstances = nInstances
						});
					},
					[](const RavEngine::World::SpotLightDataUpload& light, auto unusedAux) {

						auto lightProj = RMath::perspectiveProjection<float>(light.coneAndPenumbra.x * 2, 1, 0.1, 100);

						// -y is forward for spot lights, so we need to rotate to compensate
						auto rotmat = glm::toMat4(quaternion(vector3(-std::numbers::pi_v<float> / 2,0,0)));
						auto combinedMat = light.worldTransform * rotmat;

						auto viewMat = glm::inverse(combinedMat);

						auto camPos = light.worldTransform * glm::vec4(0, 0, 0, 1);

						return lightViewProjResult{
							.lightProj = lightProj,
							.lightView = viewMat,
							.camPos = camPos
						};
					}
				);
				mainCommandBuffer->EndRenderDebugMarker();

				mainCommandBuffer->BeginRenderDebugMarker("Render Point Lights");
				renderLight(worldOwning->renderData->pointLightData, pointLightRenderPipeline, sizeof(World::PointLightUploadData),
					[this](RGLCommandBufferPtr mainCommandBuffer){
						mainCommandBuffer->SetVertexBuffer(pointLightVertexBuffer);
						mainCommandBuffer->SetIndexBuffer(pointLightIndexBuffer);
					},
					[this](RGLCommandBufferPtr mainCommandBuffer, uint32_t nInstances) {
						mainCommandBuffer->DrawIndexed(nPointLightIndices, {
							.nInstances = nInstances
						});
					},
					[](const RavEngine::World::PointLightUploadData& light, auto unusedAux) {
						// TODO: need to do this 6 times and make a cubemap
						auto lightProj = RMath::perspectiveProjection<float>(90, 1, 0.1, 100);

						auto viewMat = glm::inverse(light.worldTransform);

						auto camPos = light.worldTransform * glm::vec4(0, 0, 0, 1);

						return lightViewProjResult{
							.lightProj = lightProj,
							.lightView = viewMat,
							.camPos = camPos
						};
					}
					);
				mainCommandBuffer->EndRenderDebugMarker();

			};

            auto renderFinalPass = [this, &target, &worldOwning, &view, &guiScaleFactor, &nextImgSize, &renderFromPerspective](auto&& viewproj, auto&& camPos, auto&& fullSizeViewport, auto&& fullSizeScissor, auto&& renderArea) {
                
                //render unlits
                unlitRenderPass->SetAttachmentTexture(0, target.lightingTexture->GetDefaultView());
                unlitRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());
                renderFromPerspective(viewproj, camPos, unlitRenderPass, [](Ref<Material>&& mat) {
                    return mat->GetMainRenderPipeline();
                }, renderArea, {.Unlit = true});
                
                // then do the skybox, if one is defined.
                mainCommandBuffer->BeginRendering(unlitRenderPass);
                if (worldOwning->skybox && worldOwning->skybox->skyMat && worldOwning->skybox->skyMat->GetMat()->renderPipeline) {
                    mainCommandBuffer->BeginRenderDebugMarker("Skybox");
                    mainCommandBuffer->BindRenderPipeline(worldOwning->skybox->skyMat->GetMat()->renderPipeline);
                    uint32_t totalIndices = 0;
                    // if a custom mesh is supplied, render that. Otherwise, render the builtin icosphere.
                    if (worldOwning->skybox->skyMesh) {
                        mainCommandBuffer->SetVertexBuffer(worldOwning->skybox->skyMesh->vertexBuffer);
                        mainCommandBuffer->SetIndexBuffer(worldOwning->skybox->skyMesh->indexBuffer);
                        totalIndices = worldOwning->skybox->skyMesh->totalIndices;
                    }
                    else {
                        mainCommandBuffer->SetVertexBuffer(pointLightVertexBuffer);
                        mainCommandBuffer->SetIndexBuffer(pointLightIndexBuffer);
                        totalIndices = nPointLightIndices;
                    }
                    mainCommandBuffer->SetVertexBytes(viewproj, 0);
                    mainCommandBuffer->DrawIndexed(totalIndices);
                    mainCommandBuffer->EndRenderDebugMarker();
                }
                mainCommandBuffer->EndRendering();
                // afterwards render the post processing effects
                uint32_t totalPostFXRendered = 0;
                RGL::TextureView currentInput = target.lightingTexture->GetDefaultView();
                RGL::TextureView altInput = target.lightingScratchTexture->GetDefaultView();
                
                for(const auto effect : globalEffects.effects){
                    if (!effect->enabled){
                        continue;
                    }
					
                    effect->Preamble({ int(fullSizeViewport.width), int(fullSizeViewport.height) });
                    for(const auto pass : effect->passes){
						BasePushConstantUBO baseUbo{
							.dim = {fullSizeViewport.width, fullSizeViewport.height}
						};
						bool isUsingFinalOutput = pass->outputConfiguration == PostProcessOutput::EngineColor;

						auto activePass = pass->clearOutputBeforeRendering ? postProcessRenderPassClear : postProcessRenderPass;

						if (isUsingFinalOutput) {
							activePass->SetAttachmentTexture(0, altInput);
						}
						else {
							activePass->SetAttachmentTexture(0, pass->outputBinding);
							auto size = pass->GetUserDefinedOutputSize();
							baseUbo.dim = { size.width, size.height };
						}
                        mainCommandBuffer->BeginRendering(activePass);
                        mainCommandBuffer->BindRenderPipeline(pass->GetEffect()->GetPipeline());
						mainCommandBuffer->SetViewport({
							.x = 0,
							.y = 0,
							.width = float(baseUbo.dim.x),
							.height = float(baseUbo.dim.y),
						});
						mainCommandBuffer->SetScissor({
							.offset = {
								0,0
							},
							.extent = {
								uint32_t(baseUbo.dim.x), 
								uint32_t(baseUbo.dim.y)
							}
						});
						{
							uint32_t index = 0;
							for (const auto& input : pass->GetinputConfiguration()) {
								if (input == PostProcessTextureInput::EngineColor) {
									mainCommandBuffer->SetFragmentTexture(currentInput, index);
								}
								else if (input == PostProcessTextureInput::UserDefined) {
									auto img = pass->inputBindings.at(index);
									mainCommandBuffer->SetFragmentTexture(img, index);
								}
								index++;
							}
						}
						{
							uint32_t index = 0;
							for (const auto sampler : pass->inputSamplerBindings) {
								if (sampler != nullptr) {
									mainCommandBuffer->SetFragmentSampler(sampler, index);
								}
								index++;
							}
						}
						
                        mainCommandBuffer->SetVertexBuffer(screenTriVerts);
                        
                        // push constants
                        std::array<std::byte, 128> pushConstants{};
                        memcpy(pushConstants.data(), &baseUbo, sizeof(baseUbo));
                        auto userPC = pass->GetPushConstantData();
                        memcpy(pushConstants.data() + sizeof(baseUbo), userPC.data(), userPC.size());
                        mainCommandBuffer->SetFragmentBytes({pushConstants.data(), userPC.size() + sizeof(baseUbo)}, 0);
                        mainCommandBuffer->Draw(3);
                        
                        mainCommandBuffer->EndRendering();
						if (isUsingFinalOutput) {
							std::swap(currentInput, altInput);
							totalPostFXRendered++;
						}
                    }
                }
                
                auto blitSource = totalPostFXRendered % 2 == 0 ? target.lightingTexture->GetDefaultView() : target.lightingScratchTexture->GetDefaultView();
                
				// the final on-screen render pass
// contains the results of the previous stages, as well as the UI, skybox and any debugging primitives
				
				glm::ivec4 viewRect {0, 0, nextImgSize.width, nextImgSize.height};

				LightToFBUBO fbubo{
					.viewRect = viewRect
				};

				mainCommandBuffer->BeginRendering(finalRenderPass);
				mainCommandBuffer->BeginRenderDebugMarker("Blit");
				// start with the results of lighting
				mainCommandBuffer->BindRenderPipeline(lightToFBRenderPipeline);
				mainCommandBuffer->SetViewport(fullSizeViewport);
				mainCommandBuffer->SetScissor(fullSizeScissor);
				mainCommandBuffer->SetVertexBuffer(screenTriVerts);
				mainCommandBuffer->SetVertexBytes(fbubo, 0);
				mainCommandBuffer->SetFragmentBytes(fbubo, 0);
				mainCommandBuffer->SetFragmentSampler(textureSampler, 0);
				mainCommandBuffer->SetFragmentTexture(blitSource, 1);
				mainCommandBuffer->Draw(3);
                mainCommandBuffer->EndRendering();

                mainCommandBuffer->BeginRendering(finalRenderPass);
				

				mainCommandBuffer->BeginRenderDebugMarker("GUI");
				worldOwning->Filter([](GUIComponent& gui) {
					gui.Render();	// kicks off commands for rendering UI
					});
#ifndef NDEBUG
				// process debug shapes
				worldOwning->FilterPolymorphic([](PolymorphicGetResult<IDebugRenderable, World::PolymorphicIndirection> dbg, const PolymorphicGetResult<Transform, World::PolymorphicIndirection> transform) {
					for (int i = 0; i < dbg.size(); i++) {
						auto& ptr = dbg[i];
						if (ptr.debugEnabled) {
							ptr.DebugDraw(dbgdraw, transform[0]);
						}
					}
					});
				mainCommandBuffer->BeginRenderDebugMarker("Debug Wireframes");
				Im3d::AppData& data = Im3d::GetAppData();
				data.m_appData = (void*)&viewproj;

				Im3d::GetContext().draw();
				mainCommandBuffer->EndRenderDebugMarker();

				if (debuggerContext) {
					auto& dbg = *debuggerContext;
					dbg.SetDimensions(view.pixelDimensions.width, view.pixelDimensions.height);
					dbg.SetDPIScale(guiScaleFactor);
					dbg.Update();
					dbg.Render();
				}

				Im3d::NewFrame();
				mainCommandBuffer->EndRenderDebugMarker();
#endif
				mainCommandBuffer->EndRendering();
			};

			auto doPassWithCamData = [this,&target,&nextImgSize](auto&& camdata, auto&& function) {
				const auto viewproj = camdata.viewProj;
				const auto invviewproj = glm::inverse(viewproj);
				const auto camPos = camdata.camPos;
				const auto viewportOverride = camdata.viewportOverride;

				RGL::Rect renderArea{
					.offset = { int32_t(nextImgSize.width * viewportOverride.originFactor.x),int32_t(nextImgSize.height * viewportOverride.originFactor.y) },
						.extent = { uint32_t(nextImgSize.width * viewportOverride.sizeFactor.x), uint32_t(nextImgSize.height * viewportOverride.sizeFactor.x) },
				};

				RGL::Viewport fullSizeViewport{
					.x = float(renderArea.offset[0]),
						.y = float(renderArea.offset[1]),
						.width = static_cast<float>(renderArea.extent[0]),
						.height = static_cast<float>(renderArea.extent[1]),
				};

				RGL::Rect fullSizeScissor{
					.offset = { 0,0 },
						.extent = { uint32_t(nextImgSize.width), uint32_t(nextImgSize.height) }
				};

				function(viewproj, camPos, fullSizeViewport, fullSizeScissor, renderArea);
			};
            
            // build the depth pyramid using the depth data from the previous frame
			depthPyramidCopyPass->SetAttachmentTexture(0, target.depthPyramidTexture->GetViewForMip(0));
				mainCommandBuffer->BeginRendering(depthPyramidCopyPass);
				mainCommandBuffer->BeginRenderDebugMarker("First copy of depth pyramid");
				mainCommandBuffer->BindRenderPipeline(depthPyramidCopyPipeline);
				mainCommandBuffer->SetViewport({0,0,float(target.pyramidSize) ,float(target.pyramidSize) });
				mainCommandBuffer->SetScissor({ 0,0,target.pyramidSize,target.pyramidSize});
				PyramidCopyUBO pubo{ .size = target.pyramidSize };
				mainCommandBuffer->SetFragmentBytes(pubo,0);
				mainCommandBuffer->SetFragmentTexture(target.depthStencil->GetDefaultView(), 0);
				mainCommandBuffer->SetFragmentSampler(depthPyramidSampler, 1);
				mainCommandBuffer->SetVertexBuffer(screenTriVerts);
				mainCommandBuffer->Draw(3);
				mainCommandBuffer->EndRenderDebugMarker();
			mainCommandBuffer->EndRendering();
            
            mainCommandBuffer->BeginCompute(depthPyramidPipeline);
			mainCommandBuffer->BeginComputeDebugMarker("Build depth pyramid");

            {
                float width = nextImgSize.width;
                float height = nextImgSize.height;
                for(int i = 0; i < target.numPyramidLevels - 1; i++){
                    auto fromTex = target.depthPyramidTexture->GetViewForMip(i);
                    auto toTex = target.depthPyramidTexture->GetViewForMip(i+1);
                    mainCommandBuffer->SetComputeTexture(toTex, 0);
                    mainCommandBuffer->SetComputeTexture(fromTex, 1);
                    mainCommandBuffer->SetComputeSampler(depthPyramidSampler, 2);
                    
                    width /= 2.0;
                    height /= 2.0;
                    
                    mainCommandBuffer->DispatchCompute(std::ceil(width/32.f), std::ceil(height/32.f), 1, 32,32,1);
                }
            }
			mainCommandBuffer->EndComputeDebugMarker();
            mainCommandBuffer->EndCompute();

			// deferred pass
			deferredRenderPass->SetAttachmentTexture(0, target.diffuseTexture->GetDefaultView());
			deferredRenderPass->SetAttachmentTexture(1, target.normalTexture->GetDefaultView());
			deferredRenderPass->SetAttachmentTexture(2, target.roughnessSpecularMetallicAOTexture->GetDefaultView());;
			deferredRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());

			deferredClearRenderPass->SetAttachmentTexture(0, target.diffuseTexture->GetDefaultView());
			deferredClearRenderPass->SetAttachmentTexture(1, target.normalTexture->GetDefaultView());
			deferredClearRenderPass->SetAttachmentTexture(2, target.roughnessSpecularMetallicAOTexture->GetDefaultView());
			deferredClearRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());

			mainCommandBuffer->BeginRenderDebugMarker("Deferred Pass");

			mainCommandBuffer->BeginRendering(deferredClearRenderPass);
			mainCommandBuffer->EndRendering();
			for (const auto& camdata : view.camDatas) {
				doPassWithCamData(camdata, renderDeferredPass);
			}
			mainCommandBuffer->EndRenderDebugMarker();
            
            if (VideoSettings.ssao){
                for (const auto& camdata : view.camDatas) {
                    //TODO: implement SSAO
                    //doPassWithCamData(camdata, renderLightingPass);
                }
            }

			// lighting pass
			mainCommandBuffer->BeginRenderDebugMarker("Lighting Pass");
			lightingRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());
			lightingRenderPass->SetAttachmentTexture(0, target.lightingTexture->GetDefaultView());
			ambientLightRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());
			ambientLightRenderPass->SetAttachmentTexture(0, target.lightingTexture->GetDefaultView());

			lightingClearRenderPass->SetAttachmentTexture(0, target.lightingTexture->GetDefaultView());
			lightingClearRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());
			mainCommandBuffer->BeginRendering(lightingClearRenderPass);	// clears the framebuffer
			mainCommandBuffer->EndRendering();
			for (const auto& camdata : view.camDatas) {
				doPassWithCamData(camdata, renderLightingPass);
			}
			mainCommandBuffer->EndRenderDebugMarker();

			// final render pass
			finalRenderPass->SetAttachmentTexture(0, target.finalFramebuffer->GetDefaultView());
			finalRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());

			finalClearRenderPass->SetAttachmentTexture(0, target.finalFramebuffer->GetDefaultView());
			finalClearRenderPass->SetDepthAttachmentTexture(target.depthStencil->GetDefaultView());

			mainCommandBuffer->BeginRenderDebugMarker("Forward Pass");

			mainCommandBuffer->BeginRendering(finalClearRenderPass);
			mainCommandBuffer->EndRendering();

			for (const auto& camdata : view.camDatas) {
				doPassWithCamData(camdata, renderFinalPass);
			}
			mainCommandBuffer->EndRenderDebugMarker();

		}
		mainCommandBuffer->End();

		auto end = std::chrono::high_resolution_clock::now();
		auto duration = duration_cast<std::chrono::microseconds>(end - start);
		currentFrameTime = duration.count();

		return mainCommandBuffer;
	}
}

void RavEngine::RenderEngine::DebugRender(const Im3d::DrawList& drawList)
{
#ifndef NDEBUG
	switch (drawList.m_primType) {
	case Im3d::DrawPrimitive_Triangles:
		mainCommandBuffer->BindRenderPipeline(im3dTriangleRenderPipeline);
		break;
	case Im3d::DrawPrimitive_Lines:
		mainCommandBuffer->BindRenderPipeline(im3dLineRenderPipeline);
		break;
	case Im3d::DrawPrimitive_Points:
		mainCommandBuffer->BindRenderPipeline(im3dPointRenderPipeline);
		break;
	default:
		Debug::Fatal("Invalid Im3d state");
		break;
	}
	//perform drawing here
	const Im3d::VertexData* vertexdata = drawList.m_vertexData;
	const auto nverts = drawList.m_vertexCount;

	auto vertBuffer = device->CreateBuffer({
		uint32_t(nverts),
		{.VertexBuffer = true},
		sizeof(Im3d::VertexData),
		RGL::BufferAccess::Private,
	});
	vertBuffer->SetBufferData({ vertexdata, nverts * sizeof(Im3d::VertexData) });

	auto viewProj = *static_cast<glm::mat4*>(Im3d::GetAppData().m_appData);
    
    DebugUBO ubo{
        .viewProj = viewProj
    };

	mainCommandBuffer->SetVertexBytes(ubo,0);
	mainCommandBuffer->SetVertexBuffer(vertBuffer);
	mainCommandBuffer->Draw(nverts);

	// trash the buffer now that we're done with it
	gcBuffers.enqueue(vertBuffer);

#endif

}
#endif
