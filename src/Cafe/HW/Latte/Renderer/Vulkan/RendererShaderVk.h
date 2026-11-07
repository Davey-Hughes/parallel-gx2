#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "util/helpers/ConcurrentQueue.h"

#include <vulkan/vulkan_core.h>
#include "util/helpers/Semaphore.h"
#include "util/helpers/AtomicPtrBag.h"

class RendererShaderVk : public RendererShader
{
	friend class VulkanRenderer;
	friend class _ShaderVkThreadPool;

	enum class COMPILATION_STATE : uint32
	{
		NONE,
		QUEUED,
		COMPILING,
		DONE
	};

public:
	static void ShaderCacheLoading_begin(uint64 cacheTitleId);
    static void ShaderCacheLoading_end();
    static void ShaderCacheLoading_Close();

	RendererShaderVk(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslCode);
	virtual ~RendererShaderVk();

	static void Init();
	static void Shutdown();

	VkShaderModule& GetShaderModule() { return m_shader_module; }

	// The pipelines using this shader, which the pipeline cache's compile
	// threads register concurrently while a title loads: no lock (one spinlock
	// for every shader before), AtomicPtrBag.h
	void TrackDependency(class PipelineInfo* p)
	{
		list_pipelineInfo.Add(p);
	}

	void RemoveDependency(class PipelineInfo* p)
	{
		list_pipelineInfo.Remove(p);
	}

	void PreponeCompilation(bool isRenderThread) override;
	bool IsCompiled() override;
	bool WaitForCompiled() override;

private:
	void CompileInternal(bool isRenderThread);

	void FinishCompilation();

	VkShaderModule m_shader_module = nullptr;

	StateSemaphore<COMPILATION_STATE> m_compilationState{ COMPILATION_STATE::NONE };

	std::string m_glslCode;

	void CreateVkShaderModule(std::span<uint32> spirvBuffer);

	// pipeline infos
	AtomicPtrBag<class PipelineInfo> list_pipelineInfo;
};
