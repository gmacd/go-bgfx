// Generate file with prepare.sh
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx.cpp"
#include "glcontext_egl.cpp"
#include "glcontext_glx.cpp"
#include "glcontext_ppapi.cpp"
#include "glcontext_wgl.cpp"
#include "image.cpp"
#include "ovr.cpp"
#include "renderdoc.cpp"
#include "renderer_d3d9.cpp"
#include "renderer_d3d11.cpp"
#include "renderer_d3d12.cpp"
#include "renderer_null.cpp"
#include "renderer_gl.cpp"
#include "renderer_vk.cpp"
#include "shader_dxbc.cpp"
#include "shader_dx9bc.cpp"
#include "shader_spirv.cpp"
#include "vertexdecl.cpp"
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

namespace bgfx
{
#define BGFX_MAIN_THREAD_MAGIC UINT32_C(0x78666762)

#if BGFX_CONFIG_MULTITHREADED && !BX_PLATFORM_OSX && !BX_PLATFORM_IOS
#	define BGFX_CHECK_MAIN_THREAD() \
				BX_CHECK(NULL != s_ctx, "Library is not initialized yet."); \
				BX_CHECK(BGFX_MAIN_THREAD_MAGIC == s_threadIndex, "Must be called from main thread.")
#	define BGFX_CHECK_RENDER_THREAD() BX_CHECK(BGFX_MAIN_THREAD_MAGIC != s_threadIndex, "Must be called from render thread.")
#else
#	define BGFX_CHECK_MAIN_THREAD()
#	define BGFX_CHECK_RENDER_THREAD()
#endif // BGFX_CONFIG_MULTITHREADED && !BX_PLATFORM_OSX && !BX_PLATFORM_IOS

#if BGFX_CONFIG_USE_TINYSTL
	void* TinyStlAllocator::static_allocate(size_t _bytes)
	{
		return BX_ALLOC(g_allocator, _bytes);
	}

	void TinyStlAllocator::static_deallocate(void* _ptr, size_t /*_bytes*/)
	{
		if (NULL != _ptr)
		{
			BX_FREE(g_allocator, _ptr);
		}
	}
#endif // BGFX_CONFIG_USE_TINYSTL

	struct CallbackStub : public CallbackI
	{
		virtual ~CallbackStub()
		{
		}

		virtual void traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList) BX_OVERRIDE
		{
			dbgPrintf("%s (%d): ", _filePath, _line);
			dbgPrintfVargs(_format, _argList);
		}

		virtual void fatal(Fatal::Enum _code, const char* _str) BX_OVERRIDE
		{
			if (Fatal::DebugCheck == _code)
			{
				bx::debugBreak();
			}
			else
			{
				BX_TRACE("0x%08x: %s", _code, _str);
				BX_UNUSED(_code, _str);
				abort();
			}
		}

		virtual uint32_t cacheReadSize(uint64_t /*_id*/) BX_OVERRIDE
		{
			return 0;
		}

		virtual bool cacheRead(uint64_t /*_id*/, void* /*_data*/, uint32_t /*_size*/) BX_OVERRIDE
		{
			return false;
		}

		virtual void cacheWrite(uint64_t /*_id*/, const void* /*_data*/, uint32_t /*_size*/) BX_OVERRIDE
		{
		}

		virtual void screenShot(const char* _filePath, uint32_t _width, uint32_t _height, uint32_t _pitch, const void* _data, uint32_t _size, bool _yflip) BX_OVERRIDE
		{
			BX_UNUSED(_filePath, _width, _height, _pitch, _data, _size, _yflip);

#if BX_CONFIG_CRT_FILE_READER_WRITER
			char* filePath = (char*)alloca(strlen(_filePath)+5);
			strcpy(filePath, _filePath);
			strcat(filePath, ".tga");

			bx::CrtFileWriter writer;
			if (0 == writer.open(filePath) )
			{
				imageWriteTga(&writer, _width, _height, _pitch, _data, false, _yflip);
				writer.close();
			}
#endif // BX_CONFIG_CRT_FILE_READER_WRITER
		}

		virtual void captureBegin(uint32_t /*_width*/, uint32_t /*_height*/, uint32_t /*_pitch*/, TextureFormat::Enum /*_format*/, bool /*_yflip*/) BX_OVERRIDE
		{
			BX_TRACE("Warning: using capture without callback (a.k.a. pointless).");
		}

		virtual void captureEnd() BX_OVERRIDE
		{
		}

		virtual void captureFrame(const void* /*_data*/, uint32_t /*_size*/) BX_OVERRIDE
		{
		}
	};

#ifndef BGFX_CONFIG_MEMORY_TRACKING
#	define BGFX_CONFIG_MEMORY_TRACKING (BGFX_CONFIG_DEBUG && BX_CONFIG_SUPPORTS_THREADING)
#endif // BGFX_CONFIG_MEMORY_TRACKING

	class AllocatorStub : public bx::ReallocatorI
	{
	public:
		AllocatorStub()
#if BGFX_CONFIG_MEMORY_TRACKING
			: m_numBlocks(0)
			, m_maxBlocks(0)
#endif // BGFX_CONFIG_MEMORY_TRACKING
		{
		}

		virtual void* alloc(size_t _size, size_t _align, const char* _file, uint32_t _line) BX_OVERRIDE
		{
			if (BX_CONFIG_ALLOCATOR_NATURAL_ALIGNMENT >= _align)
			{
#if BGFX_CONFIG_MEMORY_TRACKING
				{
					bx::LwMutexScope scope(m_mutex);
					++m_numBlocks;
					m_maxBlocks = bx::uint32_max(m_maxBlocks, m_numBlocks);
				}
#endif // BGFX_CONFIG_MEMORY_TRACKING

				return ::malloc(_size);
			}

			return bx::alignedAlloc(this, _size, _align, _file, _line);
		}

		virtual void free(void* _ptr, size_t _align, const char* _file, uint32_t _line) BX_OVERRIDE
		{
			if (NULL != _ptr)
			{
				if (BX_CONFIG_ALLOCATOR_NATURAL_ALIGNMENT >= _align)
				{
#if BGFX_CONFIG_MEMORY_TRACKING
					{
						bx::LwMutexScope scope(m_mutex);
						BX_CHECK(m_numBlocks > 0, "Number of blocks is 0. Possible alloc/free mismatch?");
						--m_numBlocks;
					}
#endif // BGFX_CONFIG_MEMORY_TRACKING

					::free(_ptr);
				}
				else
				{
					bx::alignedFree(this, _ptr, _align, _file, _line);
				}
			}
		}

		virtual void* realloc(void* _ptr, size_t _size, size_t _align, const char* _file, uint32_t _line) BX_OVERRIDE
		{
			if (BX_CONFIG_ALLOCATOR_NATURAL_ALIGNMENT >= _align)
			{
#if BGFX_CONFIG_MEMORY_TRACKING
				if (NULL == _ptr)
				{
					bx::LwMutexScope scope(m_mutex);
					++m_numBlocks;
					m_maxBlocks = bx::uint32_max(m_maxBlocks, m_numBlocks);
				}
#endif // BGFX_CONFIG_MEMORY_TRACKING

				return ::realloc(_ptr, _size);
			}

			return bx::alignedRealloc(this, _ptr, _size, _align, _file, _line);
		}

		void checkLeaks();

	protected:
#if BGFX_CONFIG_MEMORY_TRACKING
		bx::LwMutex m_mutex;
		uint32_t m_numBlocks;
		uint32_t m_maxBlocks;
#endif // BGFX_CONFIG_MEMORY_TRACKING
	};

	static CallbackStub*  s_callbackStub  = NULL;
	static AllocatorStub* s_allocatorStub = NULL;
	static bool s_graphicsDebuggerPresent = false;

	CallbackI* g_callback = NULL;
	bx::ReallocatorI* g_allocator = NULL;

	Caps g_caps;

	static BX_THREAD uint32_t s_threadIndex = 0;
	static Context* s_ctx = NULL;
	static bool s_renderFrameCalled = false;
	PlatformData g_platformData;

	void AllocatorStub::checkLeaks()
	{
#if BGFX_CONFIG_MEMORY_TRACKING
		// BK - CallbackStub will be deleted after printing this info, so there is always one
		// leak if CallbackStub is used.
		BX_WARN(uint32_t(NULL != s_callbackStub ? 1 : 0) == m_numBlocks
			, "MEMORY LEAK: %d (max: %d)"
			, m_numBlocks
			, m_maxBlocks
			);
#endif // BGFX_CONFIG_MEMORY_TRACKING
	}

	void setPlatformData(const PlatformData& _pd)
	{
		if (NULL != s_ctx)
		{
			BGFX_FATAL(true
				&& g_platformData.ndt     == _pd.ndt
				&& g_platformData.nwh     == _pd.nwh
				&& g_platformData.context == _pd.context
				, Fatal::UnableToInitialize
				, "Only backbuffer pointer can be changed after initialization!"
				);
		}
		memcpy(&g_platformData, &_pd, sizeof(PlatformData) );
	}

	void setGraphicsDebuggerPresent(bool _present)
	{
		BX_TRACE("Graphics debugger is %spresent.", _present ? "" : "not ");
		s_graphicsDebuggerPresent = _present;
	}

	bool isGraphicsDebuggerPresent()
	{
		return s_graphicsDebuggerPresent;
	}

	void fatal(Fatal::Enum _code, const char* _format, ...)
	{
		char temp[8192];

		va_list argList;
		va_start(argList, _format);
		char* out = temp;
		int32_t len = bx::vsnprintf(out, sizeof(temp), _format, argList);
		if ( (int32_t)sizeof(temp) < len)
		{
			out = (char*)alloca(len+1);
			len = bx::vsnprintf(out, len, _format, argList);
		}
		out[len] = '\0';
		va_end(argList);

		g_callback->fatal(_code, out);
	}

	void trace(const char* _filePath, uint16_t _line, const char* _format, ...)
	{
		va_list argList;
		va_start(argList, _format);
		g_callback->traceVargs(_filePath, _line, _format, argList);
		va_end(argList);

	}

#include "charset.h"

	void charsetFillTexture(const uint8_t* _charset, uint8_t* _rgba, uint32_t _height, uint32_t _pitch, uint32_t _bpp)
	{
		for (uint32_t ii = 0; ii < 256; ++ii)
		{
			uint8_t* pix = &_rgba[ii*8*_bpp];
			for (uint32_t yy = 0; yy < _height; ++yy)
			{
				for (uint32_t xx = 0; xx < 8; ++xx)
				{
					uint8_t bit = 1<<(7-xx);
					memset(&pix[xx*_bpp], _charset[ii*_height+yy]&bit ? 255 : 0, _bpp);
				}

				pix += _pitch;
			}
		}
	}

	static const uint32_t numCharsPerBatch = 1024;
	static const uint32_t numBatchVertices = numCharsPerBatch*4;
	static const uint32_t numBatchIndices = numCharsPerBatch*6;

	void TextVideoMemBlitter::init()
	{
		BGFX_CHECK_MAIN_THREAD();
		m_decl
			.begin()
			.add(Attrib::Position,  3, AttribType::Float)
			.add(Attrib::Color0,    4, AttribType::Uint8, true)
			.add(Attrib::Color1,    4, AttribType::Uint8, true)
			.add(Attrib::TexCoord0, 2, AttribType::Float)
			.end();

		uint16_t width = 2048;
		uint16_t height = 24;
		uint8_t bpp = 1;
		uint32_t pitch = width*bpp;

		const Memory* mem;

		mem = alloc(pitch*height);
		uint8_t* rgba = mem->data;
		charsetFillTexture(vga8x8, rgba, 8, pitch, bpp);
		charsetFillTexture(vga8x16, &rgba[8*pitch], 16, pitch, bpp);
		m_texture = createTexture2D(width, height, 1, TextureFormat::R8
						, BGFX_TEXTURE_MIN_POINT
						| BGFX_TEXTURE_MAG_POINT
						| BGFX_TEXTURE_MIP_POINT
						| BGFX_TEXTURE_U_CLAMP
						| BGFX_TEXTURE_V_CLAMP
						, mem
						);

		switch (g_caps.rendererType)
		{
		case RendererType::Direct3D9:
			mem = makeRef(vs_debugfont_dx9, sizeof(vs_debugfont_dx9) );
			break;

		case RendererType::Direct3D11:
		case RendererType::Direct3D12:
			mem = makeRef(vs_debugfont_dx11, sizeof(vs_debugfont_dx11) );
			break;

		case RendererType::Metal:
			mem = makeRef(vs_debugfont_mtl, sizeof(vs_debugfont_mtl) );
			break;

		default:
			mem = makeRef(vs_debugfont_glsl, sizeof(vs_debugfont_glsl) );
			break;
		}

		ShaderHandle vsh = createShader(mem);

		switch (g_caps.rendererType)
		{
		case RendererType::Direct3D9:
			mem = makeRef(fs_debugfont_dx9, sizeof(fs_debugfont_dx9) );
			break;

		case RendererType::Direct3D11:
		case RendererType::Direct3D12:
			mem = makeRef(fs_debugfont_dx11, sizeof(fs_debugfont_dx11) );
			break;

		case RendererType::Metal:
			mem = makeRef(fs_debugfont_mtl, sizeof(fs_debugfont_mtl) );
			break;

		default:
			mem = makeRef(fs_debugfont_glsl, sizeof(fs_debugfont_glsl) );
			break;
		}

		ShaderHandle fsh = createShader(mem);

		m_program = createProgram(vsh, fsh, true);

		m_vb = s_ctx->createTransientVertexBuffer(numBatchVertices*m_decl.m_stride, &m_decl);
		m_ib = s_ctx->createTransientIndexBuffer(numBatchIndices*2);
	}

	void TextVideoMemBlitter::shutdown()
	{
		BGFX_CHECK_MAIN_THREAD();

		destroyProgram(m_program);
		destroyTexture(m_texture);
		s_ctx->destroyTransientVertexBuffer(m_vb);
		s_ctx->destroyTransientIndexBuffer(m_ib);
	}

	void blit(RendererContextI* _renderCtx, TextVideoMemBlitter& _blitter, const TextVideoMem& _mem)
	{
		struct Vertex
		{
			float m_x;
			float m_y;
			float m_z;
			uint32_t m_fg;
			uint32_t m_bg;
			float m_u;
			float m_v;
		};

		static uint32_t palette[16] =
		{
			0x0,
			0xff0000cc,
			0xff069a4e,
			0xff00a0c4,
			0xffa46534,
			0xff7b5075,
			0xff9a9806,
			0xffcfd7d3,
			0xff535755,
			0xff2929ef,
			0xff34e28a,
			0xff4fe9fc,
			0xffcf9f72,
			0xffa87fad,
			0xffe2e234,
			0xffeceeee,
		};

		uint32_t yy = 0;
		uint32_t xx = 0;

		const float texelWidth = 1.0f/2048.0f;
		const float texelWidthHalf = RendererType::Direct3D9 == g_caps.rendererType ? 0.0f : texelWidth*0.5f;
		const float texelHeight = 1.0f/24.0f;
		const float texelHeightHalf = RendererType::Direct3D9 == g_caps.rendererType ? texelHeight*0.5f : 0.0f;
		const float utop = (_mem.m_small ? 0.0f : 8.0f)*texelHeight + texelHeightHalf;
		const float ubottom = (_mem.m_small ? 8.0f : 24.0f)*texelHeight + texelHeightHalf;
		const float fontHeight = (_mem.m_small ? 8.0f : 16.0f);

		_renderCtx->blitSetup(_blitter);

		for (;yy < _mem.m_height;)
		{
			Vertex* vertex = (Vertex*)_blitter.m_vb->data;
			uint16_t* indices = (uint16_t*)_blitter.m_ib->data;
			uint32_t startVertex = 0;
			uint32_t numIndices = 0;

			for (; yy < _mem.m_height && numIndices < numBatchIndices; ++yy)
			{
				xx = xx < _mem.m_width ? xx : 0;
				const uint8_t* line = &_mem.m_mem[(yy*_mem.m_width+xx)*2];

				for (; xx < _mem.m_width && numIndices < numBatchIndices; ++xx)
				{
					uint8_t ch = line[0];
					uint8_t attr = line[1];

					if (0 != (ch|attr)
					&& (' ' != ch || 0 != (attr&0xf0) ) )
					{
						uint32_t fg = palette[attr&0xf];
						uint32_t bg = palette[(attr>>4)&0xf];

						Vertex vert[4] =
						{
							{ (xx  )*8.0f, (yy  )*fontHeight, 0.0f, fg, bg, (ch  )*8.0f*texelWidth - texelWidthHalf, utop },
							{ (xx+1)*8.0f, (yy  )*fontHeight, 0.0f, fg, bg, (ch+1)*8.0f*texelWidth - texelWidthHalf, utop },
							{ (xx+1)*8.0f, (yy+1)*fontHeight, 0.0f, fg, bg, (ch+1)*8.0f*texelWidth - texelWidthHalf, ubottom },
							{ (xx  )*8.0f, (yy+1)*fontHeight, 0.0f, fg, bg, (ch  )*8.0f*texelWidth - texelWidthHalf, ubottom },
						};

						memcpy(vertex, vert, sizeof(vert) );
						vertex += 4;

						indices[0] = uint16_t(startVertex+0);
						indices[1] = uint16_t(startVertex+1);
						indices[2] = uint16_t(startVertex+2);
						indices[3] = uint16_t(startVertex+2);
						indices[4] = uint16_t(startVertex+3);
						indices[5] = uint16_t(startVertex+0);

						startVertex += 4;
						indices += 6;

						numIndices += 6;
					}

					line += 2;
				}

				if (numIndices >= numBatchIndices)
				{
					break;
				}
			}

			_renderCtx->blitRender(_blitter, numIndices);
		}
	}

	void ClearQuad::init()
	{
		BGFX_CHECK_MAIN_THREAD();

		if (RendererType::Null != g_caps.rendererType)
		{
			m_decl
				.begin()
				.add(Attrib::Position, 3, AttribType::Float)
				.end();

			ShaderHandle vsh = BGFX_INVALID_HANDLE;

			struct Mem
			{
				Mem(const void* _data, size_t _size)
					: data(_data)
					, size(_size)
				{
				}

				const void*  data;
				size_t size;
			};

			const Memory* fragMem[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS];
			if (RendererType::Direct3D9 == g_caps.rendererType)
			{
				vsh = createShader(makeRef(vs_clear_dx9, sizeof(vs_clear_dx9) ) );

				const Mem mem[] =
				{
					Mem(fs_clear0_dx9, sizeof(fs_clear0_dx9) ),
					Mem(fs_clear1_dx9, sizeof(fs_clear1_dx9) ),
					Mem(fs_clear2_dx9, sizeof(fs_clear2_dx9) ),
					Mem(fs_clear3_dx9, sizeof(fs_clear3_dx9) ),
					Mem(fs_clear4_dx9, sizeof(fs_clear4_dx9) ),
					Mem(fs_clear5_dx9, sizeof(fs_clear5_dx9) ),
					Mem(fs_clear6_dx9, sizeof(fs_clear6_dx9) ),
					Mem(fs_clear7_dx9, sizeof(fs_clear7_dx9) ),
				};

				for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
				{
					fragMem[ii] = makeRef(mem[ii].data, uint32_t(mem[ii].size) );
				}
			}
			else if (RendererType::Direct3D11 == g_caps.rendererType
				 ||  RendererType::Direct3D12 == g_caps.rendererType)
			{
				vsh = createShader(makeRef(vs_clear_dx11, sizeof(vs_clear_dx11) ) );

				const Mem mem[] =
				{
					Mem(fs_clear0_dx11, sizeof(fs_clear0_dx11) ),
					Mem(fs_clear1_dx11, sizeof(fs_clear1_dx11) ),
					Mem(fs_clear2_dx11, sizeof(fs_clear2_dx11) ),
					Mem(fs_clear3_dx11, sizeof(fs_clear3_dx11) ),
					Mem(fs_clear4_dx11, sizeof(fs_clear4_dx11) ),
					Mem(fs_clear5_dx11, sizeof(fs_clear5_dx11) ),
					Mem(fs_clear6_dx11, sizeof(fs_clear6_dx11) ),
					Mem(fs_clear7_dx11, sizeof(fs_clear7_dx11) ),
				};

				for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
				{
					fragMem[ii] = makeRef(mem[ii].data, uint32_t(mem[ii].size) );
				}
			}
			else if (RendererType::OpenGLES == g_caps.rendererType
				 ||  RendererType::OpenGL   == g_caps.rendererType)
			{
				vsh = createShader(makeRef(vs_clear_glsl, sizeof(vs_clear_glsl) ) );

				const Mem mem[] =
				{
					Mem(fs_clear0_glsl, sizeof(fs_clear0_glsl) ),
					Mem(fs_clear1_glsl, sizeof(fs_clear1_glsl) ),
					Mem(fs_clear2_glsl, sizeof(fs_clear2_glsl) ),
					Mem(fs_clear3_glsl, sizeof(fs_clear3_glsl) ),
					Mem(fs_clear4_glsl, sizeof(fs_clear4_glsl) ),
					Mem(fs_clear5_glsl, sizeof(fs_clear5_glsl) ),
					Mem(fs_clear6_glsl, sizeof(fs_clear6_glsl) ),
					Mem(fs_clear7_glsl, sizeof(fs_clear7_glsl) ),
				};

				for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
				{
					fragMem[ii] = makeRef(mem[ii].data, uint32_t(mem[ii].size) );
				}
			}
			else if (RendererType::Metal == g_caps.rendererType)
			{
				vsh = createShader(makeRef(vs_clear_mtl, sizeof(vs_clear_mtl) ) );

				const Mem mem[] =
				{
					Mem(fs_clear0_mtl, sizeof(fs_clear0_mtl) ),
					Mem(fs_clear1_mtl, sizeof(fs_clear1_mtl) ),
					Mem(fs_clear2_mtl, sizeof(fs_clear2_mtl) ),
					Mem(fs_clear3_mtl, sizeof(fs_clear3_mtl) ),
					Mem(fs_clear4_mtl, sizeof(fs_clear4_mtl) ),
					Mem(fs_clear5_mtl, sizeof(fs_clear5_mtl) ),
					Mem(fs_clear6_mtl, sizeof(fs_clear6_mtl) ),
					Mem(fs_clear7_mtl, sizeof(fs_clear7_mtl) ),
				};

				for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
				{
					fragMem[ii] = makeRef(mem[ii].data, uint32_t(mem[ii].size) );
				}
			}
			else
			{
				BGFX_FATAL(false, Fatal::UnableToInitialize, "Unknown renderer type %d", g_caps.rendererType);
			}

			for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
			{
				ShaderHandle fsh = createShader(fragMem[ii]);
				m_program[ii] = createProgram(vsh, fsh);
				BX_CHECK(isValid(m_program[ii]), "Failed to create clear quad program.");
				destroyShader(fsh);
			}

			destroyShader(vsh);

			m_vb = s_ctx->createTransientVertexBuffer(4*m_decl.m_stride, &m_decl);
		}
	}

	void ClearQuad::shutdown()
	{
		BGFX_CHECK_MAIN_THREAD();

		if (RendererType::Null != g_caps.rendererType)
		{
			for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
			{
				if (isValid(m_program[ii]) )
				{
					destroyProgram(m_program[ii]);
					m_program[ii].idx = invalidHandle;
				}
			}

			s_ctx->destroyTransientVertexBuffer(m_vb);
		}
	}

	const char* s_uniformTypeName[] =
	{
		"int1",
		NULL,
		"vec4",
		"mat3",
		"mat4",
	};
	BX_STATIC_ASSERT(UniformType::Count == BX_COUNTOF(s_uniformTypeName) );

	const char* getUniformTypeName(UniformType::Enum _enum)
	{
		BX_CHECK(_enum < UniformType::Count, "%d < UniformType::Count %d", _enum, UniformType::Count);
		return s_uniformTypeName[_enum];
	}

	UniformType::Enum nameToUniformTypeEnum(const char* _name)
	{
		for (uint32_t ii = 0; ii < UniformType::Count; ++ii)
		{
			if (NULL != s_uniformTypeName[ii]
			&&  0 == strcmp(_name, s_uniformTypeName[ii]) )
			{
				return UniformType::Enum(ii);
			}
		}

		return UniformType::Count;
	}

	static const char* s_predefinedName[PredefinedUniform::Count] =
	{
		"u_viewRect",
		"u_viewTexel",
		"u_view",
		"u_invView",
		"u_proj",
		"u_invProj",
		"u_viewProj",
		"u_invViewProj",
		"u_model",
		"u_modelView",
		"u_modelViewProj",
		"u_alphaRef4",
	};

	const char* getPredefinedUniformName(PredefinedUniform::Enum _enum)
	{
		return s_predefinedName[_enum];
	}

	PredefinedUniform::Enum nameToPredefinedUniformEnum(const char* _name)
	{
		for (uint32_t ii = 0; ii < PredefinedUniform::Count; ++ii)
		{
			if (0 == strcmp(_name, s_predefinedName[ii]) )
			{
				return PredefinedUniform::Enum(ii);
			}
		}

		return PredefinedUniform::Count;
	}

	uint32_t Frame::submit(uint8_t _id, ProgramHandle _handle, int32_t _depth)
	{
		if (m_discard)
		{
			discard();
			return m_num;
		}

		if (BGFX_CONFIG_MAX_DRAW_CALLS-1 <= m_num
		|| (0 == m_draw.m_numVertices && 0 == m_draw.m_numIndices) )
		{
			++m_numDropped;
			return m_num;
		}

		m_constEnd = m_constantBuffer->getPos();

		m_key.m_program = invalidHandle == _handle.idx
			? 0
			: _handle.idx
			;

		m_key.m_depth  = (uint32_t)_depth;
		m_key.m_view   = _id;
		m_key.m_seq    = s_ctx->m_seq[_id] & s_ctx->m_seqMask[_id];
		s_ctx->m_seq[_id]++;

		uint64_t key = m_key.encodeDraw();
		m_sortKeys[m_num]   = key;
		m_sortValues[m_num] = m_numRenderItems;
		++m_num;

		m_draw.m_constBegin = m_constBegin;
		m_draw.m_constEnd   = m_constEnd;
		m_draw.m_flags |= m_flags;
		m_renderItem[m_numRenderItems].draw = m_draw;
		++m_numRenderItems;

		m_draw.clear();
		m_constBegin = m_constEnd;
		m_flags = BGFX_STATE_NONE;

		return m_num;
	}

	uint32_t Frame::dispatch(uint8_t _id, ProgramHandle _handle, uint16_t _numX, uint16_t _numY, uint16_t _numZ, uint8_t _flags)
	{
		if (m_discard)
		{
			discard();
			return m_num;
		}

		if (BGFX_CONFIG_MAX_DRAW_CALLS-1 <= m_num)
		{
			++m_numDropped;
			return m_num;
		}

		m_constEnd = m_constantBuffer->getPos();

		m_compute.m_matrix = m_draw.m_matrix;
		m_compute.m_num    = m_draw.m_num;
		m_compute.m_numX   = bx::uint16_max(_numX, 1);
		m_compute.m_numY   = bx::uint16_max(_numY, 1);
		m_compute.m_numZ   = bx::uint16_max(_numZ, 1);
		m_compute.m_submitFlags = _flags;

		m_key.m_program = _handle.idx;
		m_key.m_depth   = 0;
		m_key.m_view    = _id;
		m_key.m_seq     = s_ctx->m_seq[_id];
		s_ctx->m_seq[_id]++;

		uint64_t key = m_key.encodeCompute();
		m_sortKeys[m_num]   = key;
		m_sortValues[m_num] = m_numRenderItems;
		++m_num;

		m_compute.m_constBegin = m_constBegin;
		m_compute.m_constEnd   = m_constEnd;
		m_renderItem[m_numRenderItems].compute = m_compute;
		++m_numRenderItems;

		m_compute.clear();
		m_constBegin = m_constEnd;

		return m_num;
	}

	void Frame::sort()
	{
		for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
		{
			m_sortKeys[ii] = SortKey::remapView(m_sortKeys[ii], m_viewRemap);
		}
		bx::radixSort64(m_sortKeys, s_ctx->m_tempKeys, m_sortValues, s_ctx->m_tempValues, m_num);
	}

	RenderFrame::Enum renderFrame()
	{
		if (BX_ENABLED(BGFX_CONFIG_MULTITHREADED) )
		{
			if (NULL == s_ctx)
			{
				s_renderFrameCalled = true;
				s_threadIndex = ~BGFX_MAIN_THREAD_MAGIC;
				return RenderFrame::NoContext;
			}

			BGFX_CHECK_RENDER_THREAD();
			if (s_ctx->renderFrame() )
			{
				Context* ctx = s_ctx;
				ctx->gameSemWait();
				s_ctx = NULL;
				ctx->renderSemPost();
				return RenderFrame::Exiting;
			}

			return RenderFrame::Render;
		}

		BX_CHECK(false, "This call only makes sense if used with multi-threaded renderer.");
		return RenderFrame::NoContext;
	}

	const uint32_t g_uniformTypeSize[UniformType::Count+1] =
	{
		sizeof(int32_t),
		0,
		4*sizeof(float),
		3*3*sizeof(float),
		4*4*sizeof(float),
		1,
	};

	void ConstantBuffer::writeUniform(UniformType::Enum _type, uint16_t _loc, const void* _value, uint16_t _num)
	{
		uint32_t opcode = encodeOpcode(_type, _loc, _num, true);
		write(opcode);
		write(_value, g_uniformTypeSize[_type]*_num);
	}

	void ConstantBuffer::writeUniformHandle(UniformType::Enum _type, uint16_t _loc, UniformHandle _handle, uint16_t _num)
	{
		uint32_t opcode = encodeOpcode(_type, _loc, _num, false);
		write(opcode);
		write(&_handle, sizeof(UniformHandle) );
	}

	void ConstantBuffer::writeMarker(const char* _marker)
	{
		uint16_t num = (uint16_t)strlen(_marker)+1;
		uint32_t opcode = encodeOpcode(bgfx::UniformType::Count, 0, num, true);
		write(opcode);
		write(_marker, num);
	}

	struct CapsFlags
	{
		uint64_t m_flag;
		const char* m_str;
	};

	static const CapsFlags s_capsFlags[] =
	{
#define CAPS_FLAGS(_x) { _x, #_x }
		CAPS_FLAGS(BGFX_CAPS_TEXTURE_COMPARE_LEQUAL),
		CAPS_FLAGS(BGFX_CAPS_TEXTURE_COMPARE_ALL),
		CAPS_FLAGS(BGFX_CAPS_TEXTURE_3D),
		CAPS_FLAGS(BGFX_CAPS_VERTEX_ATTRIB_HALF),
		CAPS_FLAGS(BGFX_CAPS_VERTEX_ATTRIB_UINT10),
		CAPS_FLAGS(BGFX_CAPS_INSTANCING),
		CAPS_FLAGS(BGFX_CAPS_RENDERER_MULTITHREADED),
		CAPS_FLAGS(BGFX_CAPS_FRAGMENT_DEPTH),
		CAPS_FLAGS(BGFX_CAPS_BLEND_INDEPENDENT),
		CAPS_FLAGS(BGFX_CAPS_COMPUTE),
		CAPS_FLAGS(BGFX_CAPS_FRAGMENT_ORDERING),
		CAPS_FLAGS(BGFX_CAPS_SWAP_CHAIN),
		CAPS_FLAGS(BGFX_CAPS_HMD),
		CAPS_FLAGS(BGFX_CAPS_INDEX32),
		CAPS_FLAGS(BGFX_CAPS_DRAW_INDIRECT),
		CAPS_FLAGS(BGFX_CAPS_HIDPI),
#undef CAPS_FLAGS
	};

	static void dumpCaps()
	{
		BX_TRACE("Supported capabilities (renderer %s, vendor 0x%04x, device 0x%04x):"
				, s_ctx->m_renderCtx->getRendererName()
				, g_caps.vendorId
				, g_caps.deviceId
				);
		for (uint32_t ii = 0; ii < BX_COUNTOF(s_capsFlags); ++ii)
		{
			if (0 != (g_caps.supported & s_capsFlags[ii].m_flag) )
			{
				BX_TRACE("\t%s", s_capsFlags[ii].m_str);
			}
		}

		BX_TRACE("Supported texture formats:");
		BX_TRACE("\t +--------- x = supported / * = emulated");
		BX_TRACE("\t |+-------- sRGB format");
		BX_TRACE("\t ||+------- vertex format");
		BX_TRACE("\t |||+------ image");
		BX_TRACE("\t ||||+----- framebuffer");
		BX_TRACE("\t |||||  +-- name");
		for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
		{
			if (TextureFormat::Unknown != ii
			&&  TextureFormat::UnknownDepth != ii)
			{
				uint8_t flags = g_caps.formats[ii];
				BX_TRACE("\t[%c%c%c%c%c] %s"
					, flags&BGFX_CAPS_FORMAT_TEXTURE_COLOR       ? 'x' : flags&BGFX_CAPS_FORMAT_TEXTURE_EMULATED ? '*' : ' '
					, flags&BGFX_CAPS_FORMAT_TEXTURE_COLOR_SRGB  ? 'l' : ' '
					, flags&BGFX_CAPS_FORMAT_TEXTURE_VERTEX      ? 'v' : ' '
					, flags&BGFX_CAPS_FORMAT_TEXTURE_IMAGE       ? 'i' : ' '
					, flags&BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER ? 'f' : ' '
					, getName(TextureFormat::Enum(ii) )
					);
				BX_UNUSED(flags);
			}
		}

		BX_TRACE("Max FB attachments: %d", g_caps.maxFBAttachments);
	}

	static TextureFormat::Enum s_emulatedFormats[] =
	{
		TextureFormat::BC1,
		TextureFormat::BC2,
		TextureFormat::BC3,
		TextureFormat::BC4,
		TextureFormat::BC5,
		TextureFormat::ETC1,
		TextureFormat::ETC2,
		TextureFormat::ETC2A,
		TextureFormat::ETC2A1,
		TextureFormat::PTC14,
		TextureFormat::PTC14A,
		TextureFormat::BGRA8, // GL doesn't support BGRA8 without extensions.
		TextureFormat::RGBA8, // D3D9 doesn't support RGBA8
	};

	bool Context::init(RendererType::Enum _type)
	{
		BX_CHECK(!m_rendererInitialized, "Already initialized?");

		m_exit   = false;
		m_frames = 0;
		m_debug  = BGFX_DEBUG_NONE;

		m_submit->create();

#if BGFX_CONFIG_MULTITHREADED
		m_render->create();

		if (s_renderFrameCalled)
		{
			// When bgfx::renderFrame is called before init render thread
			// should not be created.
			BX_TRACE("Application called bgfx::renderFrame directly, not creating render thread.");
			m_singleThreaded = true
				&& !BX_ENABLED(BX_PLATFORM_OSX || BX_PLATFORM_IOS)
				&& ~BGFX_MAIN_THREAD_MAGIC == s_threadIndex
				;
		}
		else
		{
			BX_TRACE("Creating rendering thread.");
			m_thread.init(renderThread, this);
			m_singleThreaded = false;
		}
#else
		BX_TRACE("Multithreaded renderer is disabled.");
		m_singleThreaded = true;
#endif // BGFX_CONFIG_MULTITHREADED

		BX_TRACE("Running in %s-threaded mode", m_singleThreaded ? "single" : "multi");

		s_threadIndex = BGFX_MAIN_THREAD_MAGIC;

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_viewRemap); ++ii)
		{
			m_viewRemap[ii] = uint8_t(ii);
		}

		memset(m_fb,   0xff, sizeof(m_fb) );
		memset(m_clear,   0, sizeof(m_clear) );
		memset(m_rect,    0, sizeof(m_rect) );
		memset(m_scissor, 0, sizeof(m_scissor) );
		memset(m_seq,     0, sizeof(m_seq) );
		memset(m_seqMask, 0, sizeof(m_seqMask) );

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_rect); ++ii)
		{
			m_rect[ii].m_width  = 1;
			m_rect[ii].m_height = 1;
		}

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_clearColor); ++ii)
		{
			m_clearColor[ii][0] = 0.0f;
			m_clearColor[ii][1] = 0.0f;
			m_clearColor[ii][2] = 0.0f;
			m_clearColor[ii][3] = 1.0f;
		}

		m_declRef.init();

		CommandBuffer& cmdbuf = getCommandBuffer(CommandBuffer::RendererInit);
		cmdbuf.write(_type);

		frameNoRenderWait();

		// Make sure renderer init is called from render thread.
		// g_caps is initialized and available after this point.
		frame();

		if (!m_rendererInitialized)
		{
			getCommandBuffer(CommandBuffer::RendererShutdownEnd);
			frame();
			frame();
			m_declRef.shutdown(m_vertexDeclHandle);
			m_submit->destroy();
#if BGFX_CONFIG_MULTITHREADED
			m_render->destroy();
#endif // BGFX_CONFIG_MULTITHREADED
			return false;
		}

		for (uint32_t ii = 0; ii < BX_COUNTOF(s_emulatedFormats); ++ii)
		{
			if (0 == (g_caps.formats[s_emulatedFormats[ii] ] & BGFX_CAPS_FORMAT_TEXTURE_COLOR) )
			{
				g_caps.formats[s_emulatedFormats[ii] ] |= BGFX_CAPS_FORMAT_TEXTURE_EMULATED;
			}
		}

		g_caps.rendererType = m_renderCtx->getRendererType();
		initAttribTypeSizeTable(g_caps.rendererType);

		g_caps.supported |= 0
			| (BX_ENABLED(BGFX_CONFIG_MULTITHREADED) && !m_singleThreaded ? BGFX_CAPS_RENDERER_MULTITHREADED : 0)
			;

		dumpCaps();

		m_textVideoMemBlitter.init();
		m_clearQuad.init();

		m_submit->m_transientVb = createTransientVertexBuffer(BGFX_CONFIG_TRANSIENT_VERTEX_BUFFER_SIZE);
		m_submit->m_transientIb = createTransientIndexBuffer(BGFX_CONFIG_TRANSIENT_INDEX_BUFFER_SIZE);
		frame();

		if (BX_ENABLED(BGFX_CONFIG_MULTITHREADED) )
		{
			m_submit->m_transientVb = createTransientVertexBuffer(BGFX_CONFIG_TRANSIENT_VERTEX_BUFFER_SIZE);
			m_submit->m_transientIb = createTransientIndexBuffer(BGFX_CONFIG_TRANSIENT_INDEX_BUFFER_SIZE);
			frame();
		}

		return true;
	}

	void Context::shutdown()
	{
		getCommandBuffer(CommandBuffer::RendererShutdownBegin);
		frame();

		destroyTransientVertexBuffer(m_submit->m_transientVb);
		destroyTransientIndexBuffer(m_submit->m_transientIb);
		m_textVideoMemBlitter.shutdown();
		m_clearQuad.shutdown();
		frame();

		if (BX_ENABLED(BGFX_CONFIG_MULTITHREADED) )
		{
			destroyTransientVertexBuffer(m_submit->m_transientVb);
			destroyTransientIndexBuffer(m_submit->m_transientIb);
			frame();
		}

		frame(); // If any VertexDecls needs to be destroyed.

		getCommandBuffer(CommandBuffer::RendererShutdownEnd);
		frame();

		m_dynVertexBufferAllocator.compact();
		m_dynIndexBufferAllocator.compact();

		m_declRef.shutdown(m_vertexDeclHandle);

#if BGFX_CONFIG_MULTITHREADED
		// Render thread shutdown sequence.
		renderSemWait(); // Wait for previous frame.
		gameSemPost();   // OK to set context to NULL.
		// s_ctx is NULL here.
		renderSemWait(); // In RenderFrame::Exiting state.

		if (m_thread.isRunning() )
		{
			m_thread.shutdown();
		}

		m_render->destroy();
#endif // BGFX_CONFIG_MULTITHREADED

		s_ctx = NULL;

		m_submit->destroy();

		if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
		{
#define CHECK_HANDLE_LEAK(_handleAlloc) \
					BX_MACRO_BLOCK_BEGIN \
						BX_WARN(0 == _handleAlloc.getNumHandles() \
							, "LEAK: " #_handleAlloc " %d (max: %d)" \
							, _handleAlloc.getNumHandles() \
							, _handleAlloc.getMaxHandles() \
							); \
					BX_MACRO_BLOCK_END

			CHECK_HANDLE_LEAK(m_dynamicIndexBufferHandle);
			CHECK_HANDLE_LEAK(m_dynamicVertexBufferHandle);
			CHECK_HANDLE_LEAK(m_indexBufferHandle);
			CHECK_HANDLE_LEAK(m_vertexDeclHandle);
			CHECK_HANDLE_LEAK(m_vertexBufferHandle);
			CHECK_HANDLE_LEAK(m_shaderHandle);
			CHECK_HANDLE_LEAK(m_programHandle);
			CHECK_HANDLE_LEAK(m_textureHandle);
			CHECK_HANDLE_LEAK(m_frameBufferHandle);
			CHECK_HANDLE_LEAK(m_uniformHandle);
#undef CHECK_HANDLE_LEAK
		}
	}

	void Context::freeDynamicBuffers()
	{
		for (uint16_t ii = 0, num = m_numFreeDynamicIndexBufferHandles; ii < num; ++ii)
		{
			destroyDynamicIndexBufferInternal(m_freeDynamicIndexBufferHandle[ii]);
		}
		m_numFreeDynamicIndexBufferHandles = 0;

		for (uint16_t ii = 0, num = m_numFreeDynamicVertexBufferHandles; ii < num; ++ii)
		{
			destroyDynamicVertexBufferInternal(m_freeDynamicVertexBufferHandle[ii]);
		}
		m_numFreeDynamicVertexBufferHandles = 0;
	}

	void Context::freeAllHandles(Frame* _frame)
	{
		for (uint16_t ii = 0, num = _frame->m_numFreeIndexBufferHandles; ii < num; ++ii)
		{
			m_indexBufferHandle.free(_frame->m_freeIndexBufferHandle[ii].idx);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeVertexDeclHandles; ii < num; ++ii)
		{
			m_vertexDeclHandle.free(_frame->m_freeVertexDeclHandle[ii].idx);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeVertexBufferHandles; ii < num; ++ii)
		{
			destroyVertexBufferInternal(_frame->m_freeVertexBufferHandle[ii]);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeShaderHandles; ii < num; ++ii)
		{
			m_shaderHandle.free(_frame->m_freeShaderHandle[ii].idx);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeProgramHandles; ii < num; ++ii)
		{
			m_programHandle.free(_frame->m_freeProgramHandle[ii].idx);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeTextureHandles; ii < num; ++ii)
		{
			m_textureHandle.free(_frame->m_freeTextureHandle[ii].idx);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeFrameBufferHandles; ii < num; ++ii)
		{
			m_frameBufferHandle.free(_frame->m_freeFrameBufferHandle[ii].idx);
		}

		for (uint16_t ii = 0, num = _frame->m_numFreeUniformHandles; ii < num; ++ii)
		{
			m_uniformHandle.free(_frame->m_freeUniformHandle[ii].idx);
		}
	}

	uint32_t Context::frame()
	{
		BX_CHECK(0 == m_instBufferCount, "Instance buffer allocated, but not used. This is incorrect, and causes memory leak.");

		// wait for render thread to finish
		renderSemWait();
		frameNoRenderWait();

		return m_frames;
	}

	void Context::frameNoRenderWait()
	{
		swap();

		// release render thread
		gameSemPost();
	}

	void Context::swap()
	{
		freeDynamicBuffers();
		m_submit->m_resolution = m_resolution;
		m_submit->m_debug = m_debug;

		memcpy(m_submit->m_viewRemap, m_viewRemap, sizeof(m_viewRemap) );
		memcpy(m_submit->m_fb, m_fb, sizeof(m_fb) );
		memcpy(m_submit->m_clear, m_clear, sizeof(m_clear) );
		memcpy(m_submit->m_rect, m_rect, sizeof(m_rect) );
		memcpy(m_submit->m_scissor, m_scissor, sizeof(m_scissor) );
		memcpy(m_submit->m_view, m_view, sizeof(m_view) );
		memcpy(m_submit->m_proj, m_proj, sizeof(m_proj) );
		memcpy(m_submit->m_viewFlags, m_viewFlags, sizeof(m_viewFlags) );
		if (m_clearColorDirty > 0)
		{
			--m_clearColorDirty;
			memcpy(m_submit->m_clearColor, m_clearColor, sizeof(m_clearColor) );
		}
		m_submit->finish();

		bx::xchg(m_render, m_submit);

		if (!BX_ENABLED(BGFX_CONFIG_MULTITHREADED)
		||  m_singleThreaded)
		{
			renderFrame();
		}

		m_frames++;
		m_submit->start();

		memset(m_seq, 0, sizeof(m_seq) );
		freeAllHandles(m_submit);

		m_submit->resetFreeHandles();
		m_submit->m_textVideoMem->resize(m_render->m_textVideoMem->m_small
			, m_resolution.m_width
			, m_resolution.m_height
			);
	}

	bool Context::renderFrame()
	{
		if (m_rendererInitialized
		&&  !m_flipAfterRender)
		{
			m_renderCtx->flip(m_render->m_hmd);
		}

		gameSemWait();

		rendererExecCommands(m_render->m_cmdPre);
		if (m_rendererInitialized)
		{
			m_renderCtx->submit(m_render, m_clearQuad, m_textVideoMemBlitter);
		}
		rendererExecCommands(m_render->m_cmdPost);

		renderSemPost();

		if (m_rendererInitialized
		&&  m_flipAfterRender)
		{
			m_renderCtx->flip(m_render->m_hmd);
		}

		return m_exit;
	}

	void rendererUpdateUniforms(RendererContextI* _renderCtx, ConstantBuffer* _constantBuffer, uint32_t _begin, uint32_t _end)
	{
		_constantBuffer->reset(_begin);
		while (_constantBuffer->getPos() < _end)
		{
			uint32_t opcode = _constantBuffer->read();

			if (UniformType::End == opcode)
			{
				break;
			}

			UniformType::Enum type;
			uint16_t loc;
			uint16_t num;
			uint16_t copy;
			ConstantBuffer::decodeOpcode(opcode, type, loc, num, copy);

			uint32_t size = g_uniformTypeSize[type]*num;
			const char* data = _constantBuffer->read(size);
			if (UniformType::Count > type)
			{
				if (copy)
				{
					_renderCtx->updateUniform(loc, data, size);
				}
				else
				{
					_renderCtx->updateUniform(loc, *(const char**)(data), size);
				}
			}
			else
			{
				_renderCtx->setMarker(data, size);
			}
		}
	}

	void Context::flushTextureUpdateBatch(CommandBuffer& _cmdbuf)
	{
		if (m_textureUpdateBatch.sort() )
		{
			const uint32_t pos = _cmdbuf.m_pos;

			uint32_t currentKey = UINT32_MAX;

			for (uint32_t ii = 0, num = m_textureUpdateBatch.m_num; ii < num; ++ii)
			{
				_cmdbuf.m_pos = m_textureUpdateBatch.m_values[ii];

				TextureHandle handle;
				_cmdbuf.read(handle);

				uint8_t side;
				_cmdbuf.read(side);

				uint8_t mip;
				_cmdbuf.read(mip);

				Rect rect;
				_cmdbuf.read(rect);

				uint16_t zz;
				_cmdbuf.read(zz);

				uint16_t depth;
				_cmdbuf.read(depth);

				uint16_t pitch;
				_cmdbuf.read(pitch);

				Memory* mem;
				_cmdbuf.read(mem);

				uint32_t key = m_textureUpdateBatch.m_keys[ii];
				if (key != currentKey)
				{
					if (currentKey != UINT32_MAX)
					{
						m_renderCtx->updateTextureEnd();
					}
					currentKey = key;
					m_renderCtx->updateTextureBegin(handle, side, mip);
				}

				m_renderCtx->updateTexture(handle, side, mip, rect, zz, depth, pitch, mem);

				release(mem);
			}

			if (currentKey != UINT32_MAX)
			{
				m_renderCtx->updateTextureEnd();
			}

			m_textureUpdateBatch.reset();

			_cmdbuf.m_pos = pos;
		}
	}

	typedef RendererContextI* (*RendererCreateFn)();
	typedef void (*RendererDestroyFn)();

#define BGFX_RENDERER_CONTEXT(_namespace) \
			namespace _namespace \
			{ \
				extern RendererContextI* rendererCreate(); \
				extern void rendererDestroy(); \
			}

	BGFX_RENDERER_CONTEXT(noop);
	BGFX_RENDERER_CONTEXT(d3d9);
	BGFX_RENDERER_CONTEXT(d3d11);
	BGFX_RENDERER_CONTEXT(d3d12);
	BGFX_RENDERER_CONTEXT(mtl);
	BGFX_RENDERER_CONTEXT(gl);
	BGFX_RENDERER_CONTEXT(vk);

#undef BGFX_RENDERER_CONTEXT

	struct RendererCreator
	{
		RendererCreateFn  createFn;
		RendererDestroyFn destroyFn;
		const char* name;
		bool supported;
	};

	static RendererCreator s_rendererCreator[] =
	{
		{ noop::rendererCreate,  noop::rendererDestroy,  BGFX_RENDERER_NULL_NAME,       !!BGFX_CONFIG_RENDERER_NULL       }, // Noop
		{ d3d9::rendererCreate,  d3d9::rendererDestroy,  BGFX_RENDERER_DIRECT3D9_NAME,  !!BGFX_CONFIG_RENDERER_DIRECT3D9  }, // Direct3D9
		{ d3d11::rendererCreate, d3d11::rendererDestroy, BGFX_RENDERER_DIRECT3D11_NAME, !!BGFX_CONFIG_RENDERER_DIRECT3D11 }, // Direct3D11
		{ d3d12::rendererCreate, d3d12::rendererDestroy, BGFX_RENDERER_DIRECT3D12_NAME, !!BGFX_CONFIG_RENDERER_DIRECT3D12 }, // Direct3D12
#if BX_PLATFORM_OSX || BX_PLATFORM_IOS
		{ mtl::rendererCreate,   mtl::rendererDestroy,   BGFX_RENDERER_METAL_NAME,      !!BGFX_CONFIG_RENDERER_METAL      }, // Metal
#else
		{ noop::rendererCreate,  noop::rendererDestroy,  BGFX_RENDERER_NULL_NAME,       !!BGFX_CONFIG_RENDERER_NULL       }, // Noop
#endif // BX_PLATFORM_OSX || BX_PLATFORM_IOS
		{ gl::rendererCreate,    gl::rendererDestroy,    BGFX_RENDERER_OPENGL_NAME,     !!BGFX_CONFIG_RENDERER_OPENGLES   }, // OpenGLES
		{ gl::rendererCreate,    gl::rendererDestroy,    BGFX_RENDERER_OPENGL_NAME,     !!BGFX_CONFIG_RENDERER_OPENGL     }, // OpenGL
		{ vk::rendererCreate,    vk::rendererDestroy,    BGFX_RENDERER_VULKAN_NAME,     !!BGFX_CONFIG_RENDERER_VULKAN     }, // Vulkan
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_rendererCreator) == RendererType::Count);

	static RendererDestroyFn s_rendererDestroyFn;

	struct Condition
	{
		enum Enum
		{
			LessEqual,
			GreaterEqual,
		};
	};

	bool windowsVersionIs(Condition::Enum _op, uint32_t _version)
	{
#if BX_PLATFORM_WINDOWS
		static const uint8_t s_condition[] =
		{
			VER_LESS_EQUAL,
			VER_GREATER_EQUAL,
		};

		OSVERSIONINFOEXA ovi;
		memset(&ovi, 0, sizeof(ovi) );
		ovi.dwOSVersionInfoSize = sizeof(ovi);
		// _WIN32_WINNT_WINBLUE 0x0603
		// _WIN32_WINNT_WIN8    0x0602
		// _WIN32_WINNT_WIN7    0x0601
		// _WIN32_WINNT_VISTA   0x0600
		ovi.dwMajorVersion = HIBYTE(_version);
		ovi.dwMinorVersion = LOBYTE(_version);
		DWORDLONG cond = 0;
		VER_SET_CONDITION(cond, VER_MAJORVERSION, s_condition[_op]);
		VER_SET_CONDITION(cond, VER_MINORVERSION, s_condition[_op]);
		return !!VerifyVersionInfoA(&ovi, VER_MAJORVERSION | VER_MINORVERSION, cond);
#else
		BX_UNUSED(_op, _version);
		return false;
#endif // BX_PLATFORM_WINDOWS
	}

	RendererContextI* rendererCreate(RendererType::Enum _type)
	{
		if (RendererType::Count == _type)
		{
again:
			if (BX_ENABLED(BX_PLATFORM_WINDOWS) )
			{
				RendererType::Enum first  = RendererType::Direct3D9;
				RendererType::Enum second = RendererType::Direct3D11;

				if (windowsVersionIs(Condition::GreaterEqual, 0x0602) )
				{
					first  = RendererType::Direct3D12;
					second = RendererType::Direct3D11;
					if (!s_rendererCreator[second].supported)
					{
						second = RendererType::Direct3D9;
					}
				}
				else if (windowsVersionIs(Condition::GreaterEqual, 0x0601) )
				{
					first  = RendererType::Direct3D11;
					second = RendererType::Direct3D9;
				}

				if (s_rendererCreator[first].supported)
				{
					_type = first;
				}
				else if (s_rendererCreator[second].supported)
				{
					_type = second;
				}
				else if (s_rendererCreator[RendererType::OpenGL].supported)
				{
					_type = RendererType::OpenGL;
				}
				else if (s_rendererCreator[RendererType::OpenGLES].supported)
				{
					_type = RendererType::OpenGLES;
				}
				else if (s_rendererCreator[RendererType::Direct3D12].supported)
				{
					_type = RendererType::Direct3D12;
				}
				else if (s_rendererCreator[RendererType::Vulkan].supported)
				{
					_type = RendererType::Vulkan;
				}
				else
				{
					_type = RendererType::Null;
				}
			}
			else if (BX_ENABLED(BX_PLATFORM_IOS) )
			{
				if (s_rendererCreator[RendererType::Metal].supported)
				{
					_type = RendererType::Metal;
				}
				else if (s_rendererCreator[RendererType::OpenGLES].supported)
				{
					_type = RendererType::OpenGLES;
				}
			}
			else if (BX_ENABLED(0
				 ||  BX_PLATFORM_ANDROID
				 ||  BX_PLATFORM_EMSCRIPTEN
				 ||  BX_PLATFORM_NACL
				 ||  BX_PLATFORM_RPI
				 ) )
			{
				_type = RendererType::OpenGLES;
			}
			else if (BX_ENABLED(BX_PLATFORM_WINRT) )
			{
				_type = RendererType::Direct3D11;
			}
			else
			{
				if (s_rendererCreator[RendererType::OpenGL].supported)
				{
					_type = RendererType::OpenGL;
				}
				else if (s_rendererCreator[RendererType::OpenGLES].supported)
				{
					_type = RendererType::OpenGLES;
				}
			}

			if (!s_rendererCreator[_type].supported)
			{
				_type = RendererType::Null;
			}
		}

		RendererContextI* renderCtx = s_rendererCreator[_type].createFn();

		if (NULL == renderCtx)
		{
			s_rendererCreator[_type].supported = false;
			goto again;
		}

		s_rendererDestroyFn = s_rendererCreator[_type].destroyFn;

		return renderCtx;
	}

	void rendererDestroy()
	{
		s_rendererDestroyFn();
	}

	void Context::rendererExecCommands(CommandBuffer& _cmdbuf)
	{
		_cmdbuf.reset();

		bool end = false;

		if (NULL == m_renderCtx)
		{
			uint8_t command;
			_cmdbuf.read(command);

			switch (command)
			{
			case CommandBuffer::RendererShutdownEnd:
				m_exit = true;
				return;

			case CommandBuffer::End:
				return;

			default:
				{
					BX_CHECK(CommandBuffer::RendererInit == command
						, "RendererInit must be the first command in command buffer before initialization. Unexpected command %d?"
						, command
						);
					BX_CHECK(!m_rendererInitialized, "This shouldn't happen! Bad synchronization?");

					RendererType::Enum type;
					_cmdbuf.read(type);

					m_renderCtx = rendererCreate(type);
					m_rendererInitialized = NULL != m_renderCtx;

					if (!m_rendererInitialized)
					{
						_cmdbuf.read(command);
						BX_CHECK(CommandBuffer::End == command, "Unexpected command %d?"
							, command
							);
						return;
					}
				}
				break;
			}
		}

		do
		{
			uint8_t command;
			_cmdbuf.read(command);

			switch (command)
			{
			case CommandBuffer::RendererShutdownBegin:
				{
					BX_CHECK(m_rendererInitialized, "This shouldn't happen! Bad synchronization?");
					m_rendererInitialized = false;
				}
				break;

			case CommandBuffer::RendererShutdownEnd:
				{
					BX_CHECK(!m_rendererInitialized && !m_exit, "This shouldn't happen! Bad synchronization?");
					rendererDestroy();
					m_renderCtx = NULL;
					m_exit = true;
				}
				// fall through

			case CommandBuffer::End:
				end = true;
				break;

			case CommandBuffer::CreateIndexBuffer:
				{
					IndexBufferHandle handle;
					_cmdbuf.read(handle);

					Memory* mem;
					_cmdbuf.read(mem);

					uint16_t flags;
					_cmdbuf.read(flags);

					m_renderCtx->createIndexBuffer(handle, mem, flags);

					release(mem);
				}
				break;

			case CommandBuffer::DestroyIndexBuffer:
				{
					IndexBufferHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyIndexBuffer(handle);
				}
				break;

			case CommandBuffer::CreateVertexDecl:
				{
					VertexDeclHandle handle;
					_cmdbuf.read(handle);

					VertexDecl decl;
					_cmdbuf.read(decl);

					m_renderCtx->createVertexDecl(handle, decl);
				}
				break;

			case CommandBuffer::DestroyVertexDecl:
				{
					VertexDeclHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyVertexDecl(handle);
				}
				break;

			case CommandBuffer::CreateVertexBuffer:
				{
					VertexBufferHandle handle;
					_cmdbuf.read(handle);

					Memory* mem;
					_cmdbuf.read(mem);

					VertexDeclHandle declHandle;
					_cmdbuf.read(declHandle);

					uint16_t flags;
					_cmdbuf.read(flags);

					m_renderCtx->createVertexBuffer(handle, mem, declHandle, flags);

					release(mem);
				}
				break;

			case CommandBuffer::DestroyVertexBuffer:
				{
					VertexBufferHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyVertexBuffer(handle);
				}
				break;

			case CommandBuffer::CreateDynamicIndexBuffer:
				{
					IndexBufferHandle handle;
					_cmdbuf.read(handle);

					uint32_t size;
					_cmdbuf.read(size);

					uint16_t flags;
					_cmdbuf.read(flags);

					m_renderCtx->createDynamicIndexBuffer(handle, size, flags);
				}
				break;

			case CommandBuffer::UpdateDynamicIndexBuffer:
				{
					IndexBufferHandle handle;
					_cmdbuf.read(handle);

					uint32_t offset;
					_cmdbuf.read(offset);

					uint32_t size;
					_cmdbuf.read(size);

					Memory* mem;
					_cmdbuf.read(mem);

					m_renderCtx->updateDynamicIndexBuffer(handle, offset, size, mem);

					release(mem);
				}
				break;

			case CommandBuffer::DestroyDynamicIndexBuffer:
				{
					IndexBufferHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyDynamicIndexBuffer(handle);
				}
				break;

			case CommandBuffer::CreateDynamicVertexBuffer:
				{
					VertexBufferHandle handle;
					_cmdbuf.read(handle);

					uint32_t size;
					_cmdbuf.read(size);

					uint16_t flags;
					_cmdbuf.read(flags);

					m_renderCtx->createDynamicVertexBuffer(handle, size, flags);
				}
				break;

			case CommandBuffer::UpdateDynamicVertexBuffer:
				{
					VertexBufferHandle handle;
					_cmdbuf.read(handle);

					uint32_t offset;
					_cmdbuf.read(offset);

					uint32_t size;
					_cmdbuf.read(size);

					Memory* mem;
					_cmdbuf.read(mem);

					m_renderCtx->updateDynamicVertexBuffer(handle, offset, size, mem);

					release(mem);
				}
				break;

			case CommandBuffer::DestroyDynamicVertexBuffer:
				{
					VertexBufferHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyDynamicVertexBuffer(handle);
				}
				break;

			case CommandBuffer::CreateShader:
				{
					ShaderHandle handle;
					_cmdbuf.read(handle);

					Memory* mem;
					_cmdbuf.read(mem);

					m_renderCtx->createShader(handle, mem);

					release(mem);
				}
				break;

			case CommandBuffer::DestroyShader:
				{
					ShaderHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyShader(handle);
				}
				break;

			case CommandBuffer::CreateProgram:
				{
					ProgramHandle handle;
					_cmdbuf.read(handle);

					ShaderHandle vsh;
					_cmdbuf.read(vsh);

					ShaderHandle fsh;
					_cmdbuf.read(fsh);

					m_renderCtx->createProgram(handle, vsh, fsh);
				}
				break;

			case CommandBuffer::DestroyProgram:
				{
					ProgramHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyProgram(handle);
				}
				break;

			case CommandBuffer::CreateTexture:
				{
					TextureHandle handle;
					_cmdbuf.read(handle);

					Memory* mem;
					_cmdbuf.read(mem);

					uint32_t flags;
					_cmdbuf.read(flags);

					uint8_t skip;
					_cmdbuf.read(skip);

					m_renderCtx->createTexture(handle, mem, flags, skip);

					bx::MemoryReader reader(mem->data, mem->size);

					uint32_t magic;
					bx::read(&reader, magic);

					if (BGFX_CHUNK_MAGIC_TEX == magic)
					{
						TextureCreate tc;
						bx::read(&reader, tc);

						if (NULL != tc.m_mem)
						{
							release(tc.m_mem);
						}
					}

					release(mem);
				}
				break;

			case CommandBuffer::UpdateTexture:
				{
					if (m_textureUpdateBatch.isFull() )
					{
						flushTextureUpdateBatch(_cmdbuf);
					}

					uint32_t value = _cmdbuf.m_pos;

					TextureHandle handle;
					_cmdbuf.read(handle);

					uint8_t side;
					_cmdbuf.read(side);

					uint8_t mip;
					_cmdbuf.read(mip);

					_cmdbuf.skip<Rect>();
					_cmdbuf.skip<uint16_t>();
					_cmdbuf.skip<uint16_t>();
					_cmdbuf.skip<uint16_t>();
					_cmdbuf.skip<Memory*>();

					uint32_t key = (handle.idx<<16)
						| (side<<8)
						| mip
						;

					m_textureUpdateBatch.add(key, value);
				}
				break;

			case CommandBuffer::ResizeTexture:
				{
					TextureHandle handle;
					_cmdbuf.read(handle);

					uint16_t width;
					_cmdbuf.read(width);

					uint16_t height;
					_cmdbuf.read(height);

					m_renderCtx->resizeTexture(handle, width, height);
				}
				break;

			case CommandBuffer::DestroyTexture:
				{
					TextureHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyTexture(handle);
				}
				break;

			case CommandBuffer::CreateFrameBuffer:
				{
					FrameBufferHandle handle;
					_cmdbuf.read(handle);

					bool window;
					_cmdbuf.read(window);

					if (window)
					{
						void* nwh;
						_cmdbuf.read(nwh);

						uint16_t width;
						_cmdbuf.read(width);

						uint16_t height;
						_cmdbuf.read(height);

						TextureFormat::Enum depthFormat;
						_cmdbuf.read(depthFormat);

						m_renderCtx->createFrameBuffer(handle, nwh, width, height, depthFormat);
					}
					else
					{
						uint8_t num;
						_cmdbuf.read(num);

						TextureHandle textureHandles[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS];
						for (uint32_t ii = 0; ii < num; ++ii)
						{
							_cmdbuf.read(textureHandles[ii]);
						}

						m_renderCtx->createFrameBuffer(handle, num, textureHandles);
					}
				}
				break;

			case CommandBuffer::DestroyFrameBuffer:
				{
					FrameBufferHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyFrameBuffer(handle);
				}
				break;

			case CommandBuffer::CreateUniform:
				{
					UniformHandle handle;
					_cmdbuf.read(handle);

					UniformType::Enum type;
					_cmdbuf.read(type);

					uint16_t num;
					_cmdbuf.read(num);

					uint8_t len;
					_cmdbuf.read(len);

					const char* name = (const char*)_cmdbuf.skip(len);

					m_renderCtx->createUniform(handle, type, num, name);
				}
				break;

			case CommandBuffer::DestroyUniform:
				{
					UniformHandle handle;
					_cmdbuf.read(handle);

					m_renderCtx->destroyUniform(handle);
				}
				break;

			case CommandBuffer::SaveScreenShot:
				{
					uint16_t len;
					_cmdbuf.read(len);

					const char* filePath = (const char*)_cmdbuf.skip(len);

					m_renderCtx->saveScreenShot(filePath);
				}
				break;

			case CommandBuffer::UpdateViewName:
				{
					uint8_t id;
					_cmdbuf.read(id);

					uint16_t len;
					_cmdbuf.read(len);

					const char* name = (const char*)_cmdbuf.skip(len);

					m_renderCtx->updateViewName(id, name);
				}
				break;

			default:
				BX_CHECK(false, "Invalid command: %d", command);
				break;
			}
		} while (!end);

		flushTextureUpdateBatch(_cmdbuf);
	}

	uint8_t getSupportedRenderers(RendererType::Enum _enum[RendererType::Count])
	{
		uint8_t num = 0;
		for (uint8_t ii = 0; ii < uint8_t(RendererType::Count); ++ii)
		{
			if ( (RendererType::Direct3D11 == ii || RendererType::Direct3D12 == ii)
			&&  windowsVersionIs(Condition::LessEqual, 0x0502) )
			{
				continue;
			}

			if (s_rendererCreator[ii].supported)
			{
				_enum[num++] = RendererType::Enum(ii);
			}
		}

		return num;
	}

	const char* getRendererName(RendererType::Enum _type)
	{
		BX_CHECK(_type < RendererType::Count, "Invalid renderer type %d.", _type);
		return s_rendererCreator[_type].name;
	}

	bool init(RendererType::Enum _type, uint16_t _vendorId, uint16_t _deviceId, CallbackI* _callback, bx::ReallocatorI* _allocator)
	{
		BX_CHECK(NULL == s_ctx, "bgfx is already initialized.");

		memset(&g_caps, 0, sizeof(g_caps) );
		g_caps.maxViews     = BGFX_CONFIG_MAX_VIEWS;
		g_caps.maxDrawCalls = BGFX_CONFIG_MAX_DRAW_CALLS;
		g_caps.maxFBAttachments = 1;
		g_caps.vendorId = _vendorId;
		g_caps.deviceId = _deviceId;

		if (NULL != _allocator)
		{
			g_allocator = _allocator;
		}
		else
		{
			bx::CrtAllocator allocator;
			g_allocator =
				s_allocatorStub = BX_NEW(&allocator, AllocatorStub);
		}

		if (NULL != _callback)
		{
			g_callback = _callback;
		}
		else
		{
			g_callback =
				s_callbackStub = BX_NEW(g_allocator, CallbackStub);
		}

		BX_TRACE("Init...");

		s_ctx = BX_ALIGNED_NEW(g_allocator, Context, 16);
		if (!s_ctx->init(_type) )
		{
			BX_TRACE("Init failed.");

			BX_ALIGNED_DELETE(g_allocator, s_ctx, 16);
			s_ctx = NULL;

			if (NULL != s_callbackStub)
			{
				BX_DELETE(g_allocator, s_callbackStub);
				s_callbackStub = NULL;
			}

			if (NULL != s_allocatorStub)
			{
				bx::CrtAllocator allocator;
				BX_DELETE(&allocator, s_allocatorStub);
				s_allocatorStub = NULL;
			}

			s_threadIndex = 0;
			g_callback    = NULL;
			g_allocator   = NULL;
			return false;
		}

		BX_TRACE("Init complete.");
		return true;
	}

	void shutdown()
	{
		BX_TRACE("Shutdown...");

		BGFX_CHECK_MAIN_THREAD();
		Context* ctx = s_ctx; // it's going to be NULLd inside shutdown.
		ctx->shutdown();
		BX_CHECK(NULL == s_ctx, "bgfx is should be uninitialized here.");

		BX_ALIGNED_DELETE(g_allocator, ctx, 16);

		BX_TRACE("Shutdown complete.");

		if (NULL != s_allocatorStub)
		{
			s_allocatorStub->checkLeaks();
		}

		if (NULL != s_callbackStub)
		{
			BX_DELETE(g_allocator, s_callbackStub);
			s_callbackStub = NULL;
		}

		if (NULL != s_allocatorStub)
		{
			bx::CrtAllocator allocator;
			BX_DELETE(&allocator, s_allocatorStub);
			s_allocatorStub = NULL;
		}

		s_threadIndex = 0;
		g_callback    = NULL;
		g_allocator   = NULL;
	}

	void reset(uint32_t _width, uint32_t _height, uint32_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->reset(_width, _height, _flags);
	}

	uint32_t frame()
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->frame();
	}

	const Caps* getCaps()
	{
		return &g_caps;
	}

	const HMD* getHMD()
	{
		return s_ctx->getHMD();
	}

	const Stats* getStats()
	{
		return s_ctx->getPerfStats();
	}

	RendererType::Enum getRendererType()
	{
		return g_caps.rendererType;
	}

	const Memory* alloc(uint32_t _size)
	{
		BX_CHECK(0 < _size, "Invalid memory operation. _size is 0.");
		Memory* mem = (Memory*)BX_ALLOC(g_allocator, sizeof(Memory) + _size);
		mem->size = _size;
		mem->data = (uint8_t*)mem + sizeof(Memory);
		return mem;
	}

	const Memory* copy(const void* _data, uint32_t _size)
	{
		BX_CHECK(0 < _size, "Invalid memory operation. _size is 0.");
		const Memory* mem = alloc(_size);
		memcpy(mem->data, _data, _size);
		return mem;
	}

	struct MemoryRef
	{
		Memory mem;
		ReleaseFn releaseFn;
		void* userData;
	};

	const Memory* makeRef(const void* _data, uint32_t _size, ReleaseFn _releaseFn, void* _userData)
	{
		MemoryRef* memRef = (MemoryRef*)BX_ALLOC(g_allocator, sizeof(MemoryRef) );
		memRef->mem.size  = _size;
		memRef->mem.data  = (uint8_t*)_data;
		memRef->releaseFn = _releaseFn;
		memRef->userData  = _userData;
		return &memRef->mem;
	}

	bool isMemoryRef(const Memory* _mem)
	{
		return _mem->data != (uint8_t*)_mem + sizeof(Memory);
	}

	void release(const Memory* _mem)
	{
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		Memory* mem = const_cast<Memory*>(_mem);
		if (isMemoryRef(mem) )
		{
			MemoryRef* memRef = reinterpret_cast<MemoryRef*>(mem);
			if (NULL != memRef->releaseFn)
			{
				memRef->releaseFn(mem->data, memRef->userData);
			}
		}
		BX_FREE(g_allocator, mem);
	}

	void setDebug(uint32_t _debug)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setDebug(_debug);
	}

	void dbgTextClear(uint8_t _attr, bool _small)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->dbgTextClear(_attr, _small);
	}

	void dbgTextPrintfVargs(uint16_t _x, uint16_t _y, uint8_t _attr, const char* _format, va_list _argList)
	{
		s_ctx->dbgTextPrintfVargs(_x, _y, _attr, _format, _argList);
	}

	void dbgTextPrintf(uint16_t _x, uint16_t _y, uint8_t _attr, const char* _format, ...)
	{
		BGFX_CHECK_MAIN_THREAD();
		va_list argList;
		va_start(argList, _format);
		s_ctx->dbgTextPrintfVargs(_x, _y, _attr, _format, argList);
		va_end(argList);
	}

	void dbgTextImage(uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const void* _data, uint16_t _pitch)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->dbgTextImage(_x, _y, _width, _height, _data, _pitch);
	}

	IndexBufferHandle createIndexBuffer(const Memory* _mem, uint16_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		return s_ctx->createIndexBuffer(_mem, _flags);
	}

	void destroyIndexBuffer(IndexBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyIndexBuffer(_handle);
	}

	VertexBufferHandle createVertexBuffer(const Memory* _mem, const VertexDecl& _decl, uint16_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		BX_CHECK(0 != _decl.m_stride, "Invalid VertexDecl.");
		return s_ctx->createVertexBuffer(_mem, _decl, _flags);
	}

	void destroyVertexBuffer(VertexBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyVertexBuffer(_handle);
	}

	DynamicIndexBufferHandle createDynamicIndexBuffer(uint32_t _num, uint16_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->createDynamicIndexBuffer(_num, _flags);
	}

	DynamicIndexBufferHandle createDynamicIndexBuffer(const Memory* _mem, uint16_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		return s_ctx->createDynamicIndexBuffer(_mem, _flags);
	}

	void updateDynamicIndexBuffer(DynamicIndexBufferHandle _handle, uint32_t _startIndex, const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		s_ctx->updateDynamicIndexBuffer(_handle, _startIndex, _mem);
	}

	void destroyDynamicIndexBuffer(DynamicIndexBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyDynamicIndexBuffer(_handle);
	}

	DynamicVertexBufferHandle createDynamicVertexBuffer(uint32_t _num, const VertexDecl& _decl, uint16_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(0 != _decl.m_stride, "Invalid VertexDecl.");
		return s_ctx->createDynamicVertexBuffer(_num, _decl, _flags);
	}

	DynamicVertexBufferHandle createDynamicVertexBuffer(const Memory* _mem, const VertexDecl& _decl, uint16_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		BX_CHECK(0 != _decl.m_stride, "Invalid VertexDecl.");
		return s_ctx->createDynamicVertexBuffer(_mem, _decl, _flags);
	}

	void updateDynamicVertexBuffer(DynamicVertexBufferHandle _handle, uint32_t _startVertex, const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		s_ctx->updateDynamicVertexBuffer(_handle, _startVertex, _mem);
	}

	void destroyDynamicVertexBuffer(DynamicVertexBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyDynamicVertexBuffer(_handle);
	}

	bool checkAvailTransientIndexBuffer(uint32_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(0 < _num, "Requesting 0 indices.");
		return s_ctx->checkAvailTransientIndexBuffer(_num);
	}

	bool checkAvailTransientVertexBuffer(uint32_t _num, const VertexDecl& _decl)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(0 < _num, "Requesting 0 vertices.");
		BX_CHECK(0 != _decl.m_stride, "Invalid VertexDecl.");
		return s_ctx->checkAvailTransientVertexBuffer(_num, _decl.m_stride);
	}

	bool checkAvailInstanceDataBuffer(uint32_t _num, uint16_t _stride)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(0 < _num, "Requesting 0 instances.");
		return s_ctx->checkAvailTransientVertexBuffer(_num, _stride);
	}

	bool checkAvailTransientBuffers(uint32_t _numVertices, const VertexDecl& _decl, uint32_t _numIndices)
	{
		BX_CHECK(0 != _decl.m_stride, "Invalid VertexDecl.");
		return checkAvailTransientVertexBuffer(_numVertices, _decl)
			&& checkAvailTransientIndexBuffer(_numIndices)
			;
	}

	void allocTransientIndexBuffer(TransientIndexBuffer* _tib, uint32_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _tib, "_tib can't be NULL");
		BX_CHECK(0 < _num, "Requesting 0 indices.");
		return s_ctx->allocTransientIndexBuffer(_tib, _num);
	}

	void allocTransientVertexBuffer(TransientVertexBuffer* _tvb, uint32_t _num, const VertexDecl& _decl)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _tvb, "_tvb can't be NULL");
		BX_CHECK(0 < _num, "Requesting 0 vertices.");
		BX_CHECK(UINT16_MAX >= _num, "Requesting %d vertices (max: %d).", _num, UINT16_MAX);
		BX_CHECK(0 != _decl.m_stride, "Invalid VertexDecl.");
		return s_ctx->allocTransientVertexBuffer(_tvb, _num, _decl);
	}

	bool allocTransientBuffers(bgfx::TransientVertexBuffer* _tvb, const bgfx::VertexDecl& _decl, uint32_t _numVertices, bgfx::TransientIndexBuffer* _tib, uint32_t _numIndices)
	{
		if (checkAvailTransientBuffers(_numVertices, _decl, _numIndices) )
		{
			allocTransientVertexBuffer(_tvb, _numVertices, _decl);
			allocTransientIndexBuffer(_tib, _numIndices);
			return true;
		}

		return false;
	}

	const InstanceDataBuffer* allocInstanceDataBuffer(uint32_t _num, uint16_t _stride)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(0 != (g_caps.supported & BGFX_CAPS_INSTANCING), "Instancing is not supported! Use bgfx::getCaps to check backend renderer capabilities.");
		BX_CHECK(0 < _num, "Requesting 0 instanced data vertices.");
		return s_ctx->allocInstanceDataBuffer(_num, _stride);
	}

	IndirectBufferHandle createIndirectBuffer(uint32_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->createIndirectBuffer(_num);
	}

	void destroyIndirectBuffer(IndirectBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyIndirectBuffer(_handle);
	}

	ShaderHandle createShader(const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		return s_ctx->createShader(_mem);
	}

	uint16_t getShaderUniforms(ShaderHandle _handle, UniformHandle* _uniforms, uint16_t _max)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->getShaderUniforms(_handle, _uniforms, _max);
	}

	void destroyShader(ShaderHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyShader(_handle);
	}

	ProgramHandle createProgram(ShaderHandle _vsh, ShaderHandle _fsh, bool _destroyShaders)
	{
		BGFX_CHECK_MAIN_THREAD();
		if (!isValid(_fsh) )
		{
			return createProgram(_vsh, _destroyShaders);
		}

		return s_ctx->createProgram(_vsh, _fsh, _destroyShaders);
	}

	ProgramHandle createProgram(ShaderHandle _csh, bool _destroyShader)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->createProgram(_csh, _destroyShader);
	}

	void destroyProgram(ProgramHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyProgram(_handle);
	}

	void calcTextureSize(TextureInfo& _info, uint16_t _width, uint16_t _height, uint16_t _depth, bool _cubeMap, uint8_t _numMips, TextureFormat::Enum _format)
	{
		const ImageBlockInfo& blockInfo = getBlockInfo(_format);
		const uint8_t  bpp         = blockInfo.bitsPerPixel;
		const uint16_t blockWidth  = blockInfo.blockWidth;
		const uint16_t blockHeight = blockInfo.blockHeight;
		const uint16_t minBlockX   = blockInfo.minBlockX;
		const uint16_t minBlockY   = blockInfo.minBlockY;

		_width   = bx::uint16_max(blockWidth  * minBlockX, ( (_width  + blockWidth  - 1) / blockWidth)*blockWidth);
		_height  = bx::uint16_max(blockHeight * minBlockY, ( (_height + blockHeight - 1) / blockHeight)*blockHeight);
		_depth   = bx::uint16_max(1, _depth);
		_numMips = uint8_t(bx::uint16_max(1, _numMips) );

		uint32_t width  = _width;
		uint32_t height = _height;
		uint32_t depth  = _depth;
		uint32_t sides  = _cubeMap ? 6 : 1;
		uint32_t size   = 0;

		for (uint32_t lod = 0; lod < _numMips; ++lod)
		{
			width  = bx::uint32_max(blockWidth  * minBlockX, ( (width  + blockWidth  - 1) / blockWidth )*blockWidth);
			height = bx::uint32_max(blockHeight * minBlockY, ( (height + blockHeight - 1) / blockHeight)*blockHeight);
			depth  = bx::uint32_max(1, depth);

			size += width*height*depth*bpp/8 * sides;

			width  >>= 1;
			height >>= 1;
			depth  >>= 1;
		}

		_info.format  = _format;
		_info.width   = _width;
		_info.height  = _height;
		_info.depth   = _depth;
		_info.numMips = _numMips;
		_info.cubeMap = _cubeMap;
		_info.storageSize  = size;
		_info.bitsPerPixel = bpp;
	}

	TextureHandle createTexture(const Memory* _mem, uint32_t _flags, uint8_t _skip, TextureInfo* _info)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		return s_ctx->createTexture(_mem, _flags, _skip, _info, BackbufferRatio::Count);
	}

	void getTextureSizeFromRatio(BackbufferRatio::Enum _ratio, uint16_t& _width, uint16_t& _height)
	{
		switch (_ratio)
		{
		case BackbufferRatio::Half:      _width /=  2; _height /=  2; break;
		case BackbufferRatio::Quarter:   _width /=  4; _height /=  4; break;
		case BackbufferRatio::Eighth:    _width /=  8; _height /=  8; break;
		case BackbufferRatio::Sixteenth: _width /= 16; _height /= 16; break;
		case BackbufferRatio::Double:    _width *=  2; _height *=  2; break;

		default:
			break;
		}

		_width  = bx::uint16_max(1, _width);
		_height = bx::uint16_max(1, _height);
	}

	TextureHandle createTexture2D(BackbufferRatio::Enum _ratio, uint16_t _width, uint16_t _height, uint8_t _numMips, TextureFormat::Enum _format, uint32_t _flags, const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();

		_numMips = uint8_t(bx::uint32_max(1, _numMips) );

		if (BX_ENABLED(BGFX_CONFIG_DEBUG)
		&&  NULL != _mem)
		{
			TextureInfo ti;
			calcTextureSize(ti, _width, _height, 1, false, _numMips, _format);
			BX_CHECK(ti.storageSize == _mem->size
				, "createTexture2D: Texture storage size doesn't match passed memory size (storage size: %d, memory size: %d)"
				, ti.storageSize
				, _mem->size
				);
		}

		uint32_t size = sizeof(uint32_t)+sizeof(TextureCreate);
		const Memory* mem = alloc(size);

		bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
		uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
		bx::write(&writer, magic);

		if (BackbufferRatio::Count != _ratio)
		{
			_width  = uint16_t(s_ctx->m_frame->m_resolution.m_width);
			_height = uint16_t(s_ctx->m_frame->m_resolution.m_height);
			getTextureSizeFromRatio(_ratio, _width, _height);
		}

		TextureCreate tc;
		tc.m_flags   = _flags;
		tc.m_width   = _width;
		tc.m_height  = _height;
		tc.m_sides   = 0;
		tc.m_depth   = 0;
		tc.m_numMips = _numMips;
		tc.m_format  = uint8_t(_format);
		tc.m_cubeMap = false;
		tc.m_mem     = _mem;
		bx::write(&writer, tc);

		return s_ctx->createTexture(mem, _flags, 0, NULL, _ratio);
	}

	TextureHandle createTexture2D(uint16_t _width, uint16_t _height, uint8_t _numMips, TextureFormat::Enum _format, uint32_t _flags, const Memory* _mem)
	{
		BX_CHECK(_width > 0 && _height > 0, "Invalid texture size (width %d, height %d).", _width, _height);
		return createTexture2D(BackbufferRatio::Count, _width, _height, _numMips, _format, _flags, _mem);
	}

	TextureHandle createTexture2D(BackbufferRatio::Enum _ratio, uint8_t _numMips, TextureFormat::Enum _format, uint32_t _flags)
	{
		BX_CHECK(_ratio < BackbufferRatio::Count, "Invalid back buffer ratio.");
		return createTexture2D(_ratio, 0, 0, _numMips, _format, _flags, NULL);
	}

	TextureHandle createTexture3D(uint16_t _width, uint16_t _height, uint16_t _depth, uint8_t _numMips, TextureFormat::Enum _format, uint32_t _flags, const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(0 != (g_caps.supported & BGFX_CAPS_TEXTURE_3D), "Texture3D is not supported! Use bgfx::getCaps to check backend renderer capabilities.");

		_numMips = uint8_t(bx::uint32_max(1, _numMips) );

		if (BX_ENABLED(BGFX_CONFIG_DEBUG)
		&&  NULL != _mem)
		{
			TextureInfo ti;
			calcTextureSize(ti, _width, _height, _depth, false, _numMips, _format);
			BX_CHECK(ti.storageSize == _mem->size
				, "createTexture3D: Texture storage size doesn't match passed memory size (storage size: %d, memory size: %d)"
				, ti.storageSize
				, _mem->size
				);
		}

		uint32_t size = sizeof(uint32_t)+sizeof(TextureCreate);
		const Memory* mem = alloc(size);

		bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
		uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
		bx::write(&writer, magic);

		TextureCreate tc;
		tc.m_flags = _flags;
		tc.m_width = _width;
		tc.m_height = _height;
		tc.m_sides = 0;
		tc.m_depth = _depth;
		tc.m_numMips = _numMips;
		tc.m_format = uint8_t(_format);
		tc.m_cubeMap = false;
		tc.m_mem = _mem;
		bx::write(&writer, tc);

		return s_ctx->createTexture(mem, _flags, 0, NULL, BackbufferRatio::Count);
	}

	TextureHandle createTextureCube(uint16_t _size, uint8_t _numMips, TextureFormat::Enum _format, uint32_t _flags, const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();

		_numMips = uint8_t(bx::uint32_max(1, _numMips) );

		if (BX_ENABLED(BGFX_CONFIG_DEBUG)
		&&  NULL != _mem)
		{
			TextureInfo ti;
			calcTextureSize(ti, _size, _size, 1, true, _numMips, _format);
			BX_CHECK(ti.storageSize == _mem->size
				, "createTextureCube: Texture storage size doesn't match passed memory size (storage size: %d, memory size: %d)"
				, ti.storageSize
				, _mem->size
				);
		}

		uint32_t size = sizeof(uint32_t)+sizeof(TextureCreate);
		const Memory* mem = alloc(size);

		bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
		uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
		bx::write(&writer, magic);

		TextureCreate tc;
		tc.m_flags   = _flags;
		tc.m_width   = _size;
		tc.m_height  = _size;
		tc.m_sides   = 6;
		tc.m_depth   = 0;
		tc.m_numMips = _numMips;
		tc.m_format  = uint8_t(_format);
		tc.m_cubeMap = true;
		tc.m_mem     = _mem;
		bx::write(&writer, tc);

		return s_ctx->createTexture(mem, _flags, 0, NULL, BackbufferRatio::Count);
	}

	void destroyTexture(TextureHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyTexture(_handle);
	}

	void updateTexture2D(TextureHandle _handle, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const Memory* _mem, uint16_t _pitch)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		if (_width == 0
		||  _height == 0)
		{
			release(_mem);
		}
		else
		{
			s_ctx->updateTexture(_handle, 0, _mip, _x, _y, 0, _width, _height, 1, _pitch, _mem);
		}
	}

	void updateTexture3D(TextureHandle _handle, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _z, uint16_t _width, uint16_t _height, uint16_t _depth, const Memory* _mem)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		if (_width == 0
		||  _height == 0
		||  _depth == 0)
		{
			release(_mem);
		}
		else
		{
			s_ctx->updateTexture(_handle, 0, _mip, _x, _y, _z, _width, _height, _depth, UINT16_MAX, _mem);
		}
	}

	void updateTextureCube(TextureHandle _handle, uint8_t _side, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const Memory* _mem, uint16_t _pitch)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _mem, "_mem can't be NULL");
		BX_CHECK(_side <= 5, "Invalid side %d.", _side);
		if (_width == 0
		||  _height == 0)
		{
			release(_mem);
		}
		else
		{
			s_ctx->updateTexture(_handle, _side, _mip, _x, _y, 0, _width, _height, 1, _pitch, _mem);
		}
	}

	FrameBufferHandle createFrameBuffer(uint16_t _width, uint16_t _height, TextureFormat::Enum _format, uint32_t _textureFlags)
	{
		_textureFlags |= _textureFlags&BGFX_TEXTURE_RT_MSAA_MASK ? 0 : BGFX_TEXTURE_RT;
		TextureHandle th = createTexture2D(_width, _height, 1, _format, _textureFlags);
		return createFrameBuffer(1, &th, true);
	}

	FrameBufferHandle createFrameBuffer(BackbufferRatio::Enum _ratio, TextureFormat::Enum _format, uint32_t _textureFlags)
	{
		BX_CHECK(_ratio < BackbufferRatio::Count, "Invalid back buffer ratio.");
		_textureFlags |= _textureFlags&BGFX_TEXTURE_RT_MSAA_MASK ? 0 : BGFX_TEXTURE_RT;
		TextureHandle th = createTexture2D(_ratio, 1, _format, _textureFlags);
		return createFrameBuffer(1, &th, true);
	}

	FrameBufferHandle createFrameBuffer(uint8_t _num, TextureHandle* _handles, bool _destroyTextures)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(_num != 0, "Number of frame buffer attachments can't be 0.");
		BX_CHECK(_num <= BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS, "Number of frame buffer attachments is larger than allowed %d (max: %d)."
			, _num
			, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS
			);
		BX_CHECK(NULL != _handles, "_handles can't be NULL");
		return s_ctx->createFrameBuffer(_num, _handles, _destroyTextures);
	}

	FrameBufferHandle createFrameBuffer(void* _nwh, uint16_t _width, uint16_t _height, TextureFormat::Enum _depthFormat)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->createFrameBuffer(_nwh, _width, _height, _depthFormat);
	}

	void destroyFrameBuffer(FrameBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyFrameBuffer(_handle);
	}

	UniformHandle createUniform(const char* _name, UniformType::Enum _type, uint16_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->createUniform(_name, _type, _num);
	}

	void destroyUniform(UniformHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->destroyUniform(_handle);
	}

	void setClearColor(uint8_t _index, uint32_t _rgba)
	{
		BGFX_CHECK_MAIN_THREAD();

		const uint8_t rr = uint8_t(_rgba>>24);
		const uint8_t gg = uint8_t(_rgba>>16);
		const uint8_t bb = uint8_t(_rgba>> 8);
		const uint8_t aa = uint8_t(_rgba>> 0);

		float rgba[4] =
		{
			rr * 1.0f/255.0f,
			gg * 1.0f/255.0f,
			bb * 1.0f/255.0f,
			aa * 1.0f/255.0f,
		};
		s_ctx->setClearColor(_index, rgba);
	}

	void setClearColor(uint8_t _index, float _r, float _g, float _b, float _a)
	{
		BGFX_CHECK_MAIN_THREAD();
		float rgba[4] = { _r, _g, _b, _a };
		s_ctx->setClearColor(_index, rgba);
	}

	void setClearColor(uint8_t _index, const float _rgba[4])
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setClearColor(_index, _rgba);
	}

	bool checkView(uint8_t _id)
	{
		// workaround GCC 4.9 type-limit check.
		const uint32_t id = _id;
		return id < BGFX_CONFIG_MAX_VIEWS;
	}

	void setViewName(uint8_t _id, const char* _name)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewName(_id, _name);
	}

	void setViewRect(uint8_t _id, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewRect(_id, _x, _y, _width, _height);
	}

	void setViewScissor(uint8_t _id, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewScissor(_id, _x, _y, _width, _height);
	}

	void setViewClear(uint8_t _id, uint16_t _flags, uint32_t _rgba, float _depth, uint8_t _stencil)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewClear(_id, _flags, _rgba, _depth, _stencil);
	}

	void setViewClear(uint8_t _id, uint16_t _flags, float _depth, uint8_t _stencil, uint8_t _0, uint8_t _1, uint8_t _2, uint8_t _3, uint8_t _4, uint8_t _5, uint8_t _6, uint8_t _7)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewClear(_id, _flags, _depth, _stencil, _0, _1, _2, _3, _4, _5, _6, _7);
	}

	void setViewSeq(uint8_t _id, bool _enabled)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewSeq(_id, _enabled);
	}

	void setViewFrameBuffer(uint8_t _id, FrameBufferHandle _handle)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewFrameBuffer(_id, _handle);
	}

	void setViewTransform(uint8_t _id, const void* _view, const void* _projL, uint8_t _flags, const void* _projR)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewTransform(_id, _view, _projL, _flags, _projR);
	}

	void setViewRemap(uint8_t _id, uint8_t _num, const void* _remap)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(checkView(_id), "Invalid view id: %d", _id);
		s_ctx->setViewRemap(_id, _num, _remap);
	}

	void setMarker(const char* _marker)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setMarker(_marker);
	}

	void setState(uint64_t _state, uint32_t _rgba)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setState(_state, _rgba);
	}

	void setStencil(uint32_t _fstencil, uint32_t _bstencil)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setStencil(_fstencil, _bstencil);
	}

	uint16_t setScissor(uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->setScissor(_x, _y, _width, _height);
	}

	void setScissor(uint16_t _cache)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setScissor(_cache);
	}

	uint32_t setTransform(const void* _mtx, uint16_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->setTransform(_mtx, _num);
	}

	uint32_t allocTransform(Transform* _transform, uint16_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->allocTransform(_transform, _num);
	}

	void setTransform(uint32_t _cache, uint16_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setTransform(_cache, _num);
	}

	void setUniform(UniformHandle _handle, const void* _value, uint16_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setUniform(_handle, _value, _num);
	}

	void setIndexBuffer(IndexBufferHandle _handle, uint32_t _firstIndex, uint32_t _numIndices)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setIndexBuffer(_handle, _firstIndex, _numIndices);
	}

	void setIndexBuffer(DynamicIndexBufferHandle _handle, uint32_t _firstIndex, uint32_t _numIndices)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setIndexBuffer(_handle, _firstIndex, _numIndices);
	}

	void setIndexBuffer(const TransientIndexBuffer* _tib)
	{
		setIndexBuffer(_tib, 0, UINT32_MAX);
	}

	void setIndexBuffer(const TransientIndexBuffer* _tib, uint32_t _firstIndex, uint32_t _numIndices)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _tib, "_tib can't be NULL");
		uint32_t numIndices = bx::uint32_min(_numIndices, _tib->size/2);
		s_ctx->setIndexBuffer(_tib, _tib->startIndex + _firstIndex, numIndices);
	}

	void setVertexBuffer(VertexBufferHandle _handle)
	{
		setVertexBuffer(_handle, 0, UINT32_MAX);
	}

	void setVertexBuffer(VertexBufferHandle _handle, uint32_t _startVertex, uint32_t _numVertices)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setVertexBuffer(_handle, _startVertex, _numVertices);
	}

	void setVertexBuffer(DynamicVertexBufferHandle _handle, uint32_t _numVertices)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setVertexBuffer(_handle, _numVertices);
	}

	void setVertexBuffer(const TransientVertexBuffer* _tvb)
	{
		setVertexBuffer(_tvb, 0, UINT32_MAX);
	}

	void setVertexBuffer(const TransientVertexBuffer* _tvb, uint32_t _startVertex, uint32_t _numVertices)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _tvb, "_tvb can't be NULL");
		s_ctx->setVertexBuffer(_tvb, _startVertex, _numVertices);
	}

	void setInstanceDataBuffer(const InstanceDataBuffer* _idb, uint32_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		BX_CHECK(NULL != _idb, "_idb can't be NULL");
		s_ctx->setInstanceDataBuffer(_idb, _num);
	}

	void setInstanceDataBuffer(VertexBufferHandle _handle, uint32_t _startVertex, uint32_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setInstanceDataBuffer(_handle, _startVertex, _num);
	}

	void setInstanceDataBuffer(DynamicVertexBufferHandle _handle, uint32_t _startVertex, uint32_t _num)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setInstanceDataBuffer(_handle, _startVertex, _num);
	}

	void setTexture(uint8_t _stage, UniformHandle _sampler, TextureHandle _handle, uint32_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setTexture(_stage, _sampler, _handle, _flags);
	}

	void setTexture(uint8_t _stage, UniformHandle _sampler, FrameBufferHandle _handle, uint8_t _attachment, uint32_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setTexture(_stage, _sampler, _handle, _attachment, _flags);
	}

	uint32_t touch(uint8_t _id)
	{
		ProgramHandle handle = BGFX_INVALID_HANDLE;
		return submit(_id, handle);
	}

	uint32_t submit(uint8_t _id, ProgramHandle _handle, int32_t _depth)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->submit(_id, _handle, _depth);
	}

	uint32_t submit(uint8_t _id, ProgramHandle _handle, IndirectBufferHandle _indirectHandle, uint16_t _start, uint16_t _num, int32_t _depth)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->submit(_id, _handle, _indirectHandle, _start, _num, _depth);
	}

	void setBuffer(uint8_t _stage, IndexBufferHandle _handle, Access::Enum _access)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setBuffer(_stage, _handle, _access);
	}

	void setBuffer(uint8_t _stage, VertexBufferHandle _handle, Access::Enum _access)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setBuffer(_stage, _handle, _access);
	}

	void setBuffer(uint8_t _stage, DynamicIndexBufferHandle _handle, Access::Enum _access)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setBuffer(_stage, _handle, _access);
	}

	void setBuffer(uint8_t _stage, DynamicVertexBufferHandle _handle, Access::Enum _access)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setBuffer(_stage, _handle, _access);
	}

	void setBuffer(uint8_t _stage, IndirectBufferHandle _handle, Access::Enum _access)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setBuffer(_stage, _handle, _access);
	}

	void setImage(uint8_t _stage, UniformHandle _sampler, TextureHandle _handle, uint8_t _mip, Access::Enum _access, TextureFormat::Enum _format)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setImage(_stage, _sampler, _handle, _mip, _access, _format);
	}

	void setImage(uint8_t _stage, UniformHandle _sampler, FrameBufferHandle _handle, uint8_t _attachment, Access::Enum _access, TextureFormat::Enum _format)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->setImage(_stage, _sampler, _handle, _attachment, _access, _format);
	}

	uint32_t dispatch(uint8_t _id, ProgramHandle _handle, uint16_t _numX, uint16_t _numY, uint16_t _numZ, uint8_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->dispatch(_id, _handle, _numX, _numY, _numZ, _flags);
	}

	uint32_t dispatch(uint8_t _id, ProgramHandle _handle, IndirectBufferHandle _indirectHandle, uint16_t _start, uint16_t _num, uint8_t _flags)
	{
		BGFX_CHECK_MAIN_THREAD();
		return s_ctx->dispatch(_id, _handle, _indirectHandle, _start, _num, _flags);
	}

	void discard()
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->discard();
	}

	void saveScreenShot(const char* _filePath)
	{
		BGFX_CHECK_MAIN_THREAD();
		s_ctx->saveScreenShot(_filePath);
	}
} // namespace bgfx

#include <bgfx.c99.h>
#include <bgfxplatform.c99.h>

BX_STATIC_ASSERT(bgfx::Fatal::Count         == bgfx::Fatal::Enum(BGFX_FATAL_COUNT) );
BX_STATIC_ASSERT(bgfx::RendererType::Count  == bgfx::RendererType::Enum(BGFX_RENDERER_TYPE_COUNT) );
BX_STATIC_ASSERT(bgfx::Attrib::Count        == bgfx::Attrib::Enum(BGFX_ATTRIB_COUNT) );
BX_STATIC_ASSERT(bgfx::AttribType::Count    == bgfx::AttribType::Enum(BGFX_ATTRIB_TYPE_COUNT) );
BX_STATIC_ASSERT(bgfx::TextureFormat::Count == bgfx::TextureFormat::Enum(BGFX_TEXTURE_FORMAT_COUNT) );
BX_STATIC_ASSERT(bgfx::UniformType::Count   == bgfx::UniformType::Enum(BGFX_UNIFORM_TYPE_COUNT) );
BX_STATIC_ASSERT(bgfx::RenderFrame::Count   == bgfx::RenderFrame::Enum(BGFX_RENDER_FRAME_COUNT) );

BX_STATIC_ASSERT(sizeof(bgfx::Memory)                == sizeof(bgfx_memory_t) );
BX_STATIC_ASSERT(sizeof(bgfx::VertexDecl)            == sizeof(bgfx_vertex_decl_t) );
BX_STATIC_ASSERT(sizeof(bgfx::TransientIndexBuffer)  == sizeof(bgfx_transient_index_buffer_t) );
BX_STATIC_ASSERT(sizeof(bgfx::TransientVertexBuffer) == sizeof(bgfx_transient_vertex_buffer_t) );
BX_STATIC_ASSERT(sizeof(bgfx::InstanceDataBuffer)    == sizeof(bgfx_instance_data_buffer_t) );
BX_STATIC_ASSERT(sizeof(bgfx::TextureInfo)           == sizeof(bgfx_texture_info_t) );
BX_STATIC_ASSERT(sizeof(bgfx::Caps)                  == sizeof(bgfx_caps_t) );
BX_STATIC_ASSERT(sizeof(bgfx::PlatformData)          == sizeof(bgfx_platform_data_t) );

namespace bgfx
{
	struct CallbackC99 : public CallbackI
	{
		virtual ~CallbackC99()
		{
		}

		virtual void fatal(Fatal::Enum _code, const char* _str) BX_OVERRIDE
		{
			m_interface->vtbl->fatal(m_interface, (bgfx_fatal_t)_code, _str);
		}

		virtual void traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList) BX_OVERRIDE
		{
			m_interface->vtbl->trace_vargs(m_interface, _filePath, _line, _format, _argList);
		}

		virtual uint32_t cacheReadSize(uint64_t _id) BX_OVERRIDE
		{
			return m_interface->vtbl->cache_read_size(m_interface, _id);
		}

		virtual bool cacheRead(uint64_t _id, void* _data, uint32_t _size) BX_OVERRIDE
		{
			return m_interface->vtbl->cache_read(m_interface, _id, _data, _size);
		}

		virtual void cacheWrite(uint64_t _id, const void* _data, uint32_t _size) BX_OVERRIDE
		{
			m_interface->vtbl->cache_write(m_interface, _id, _data, _size);
		}

		virtual void screenShot(const char* _filePath, uint32_t _width, uint32_t _height, uint32_t _pitch, const void* _data, uint32_t _size, bool _yflip) BX_OVERRIDE
		{
			m_interface->vtbl->screen_shot(m_interface, _filePath, _width, _height, _pitch, _data, _size, _yflip);
		}

		virtual void captureBegin(uint32_t _width, uint32_t _height, uint32_t _pitch, TextureFormat::Enum _format, bool _yflip) BX_OVERRIDE
		{
			m_interface->vtbl->capture_begin(m_interface, _width, _height, _pitch, (bgfx_texture_format_t)_format, _yflip);
		}

		virtual void captureEnd() BX_OVERRIDE
		{
			m_interface->vtbl->capture_end(m_interface);
		}

		virtual void captureFrame(const void* _data, uint32_t _size) BX_OVERRIDE
		{
			m_interface->vtbl->capture_frame(m_interface, _data, _size);
		}

		bgfx_callback_interface_t* m_interface;
	};

	class AllocatorC99 : public bx::ReallocatorI
	{
	public:
		virtual ~AllocatorC99()
		{
		}

		virtual void* alloc(size_t _size, size_t _align, const char* _file, uint32_t _line) BX_OVERRIDE
		{
			return m_interface->vtbl->alloc(m_interface, _size, _align, _file, _line);
		}

		virtual void free(void* _ptr, size_t _align, const char* _file, uint32_t _line) BX_OVERRIDE
		{
			m_interface->vtbl->free(m_interface, _ptr, _align, _file, _line);
		}

		virtual void* realloc(void* _ptr, size_t _size, size_t _align, const char* _file, uint32_t _line) BX_OVERRIDE
		{
			return m_interface->vtbl->realloc(m_interface, _ptr, _size, _align, _file, _line);
		}

		bgfx_reallocator_interface_t* m_interface;
	};

} // namespace bgfx

BGFX_C_API void bgfx_vertex_decl_begin(bgfx_vertex_decl_t* _decl, bgfx_renderer_type_t _renderer)
{
	bgfx::VertexDecl* decl = (bgfx::VertexDecl*)_decl;
	decl->begin(bgfx::RendererType::Enum(_renderer) );
}

BGFX_C_API void bgfx_vertex_decl_add(bgfx_vertex_decl_t* _decl, bgfx_attrib_t _attrib, uint8_t _num, bgfx_attrib_type_t _type, bool _normalized, bool _asInt)
{
	bgfx::VertexDecl* decl = (bgfx::VertexDecl*)_decl;
	decl->add(bgfx::Attrib::Enum(_attrib)
		, _num
		, bgfx::AttribType::Enum(_type)
		, _normalized
		, _asInt
		);
}

BGFX_C_API void bgfx_vertex_decl_skip(bgfx_vertex_decl_t* _decl, uint8_t _num)
{
	bgfx::VertexDecl* decl = (bgfx::VertexDecl*)_decl;
	decl->skip(_num);
}

BGFX_C_API void bgfx_vertex_decl_end(bgfx_vertex_decl_t* _decl)
{
	bgfx::VertexDecl* decl = (bgfx::VertexDecl*)_decl;
	decl->end();
}

BGFX_C_API void bgfx_vertex_pack(const float _input[4], bool _inputNormalized, bgfx_attrib_t _attr, const bgfx_vertex_decl_t* _decl, void* _data, uint32_t _index)
{
	bgfx::VertexDecl& decl = *(bgfx::VertexDecl*)_decl;
	bgfx::vertexPack(_input, _inputNormalized, bgfx::Attrib::Enum(_attr), decl, _data, _index);
}

BGFX_C_API void bgfx_vertex_unpack(float _output[4], bgfx_attrib_t _attr, const bgfx_vertex_decl_t* _decl, const void* _data, uint32_t _index)
{
	bgfx::VertexDecl& decl = *(bgfx::VertexDecl*)_decl;
	bgfx::vertexUnpack(_output, bgfx::Attrib::Enum(_attr), decl, _data, _index);
}

BGFX_C_API void bgfx_vertex_convert(const bgfx_vertex_decl_t* _destDecl, void* _destData, const bgfx_vertex_decl_t* _srcDecl, const void* _srcData, uint32_t _num)
{
	bgfx::VertexDecl& destDecl = *(bgfx::VertexDecl*)_destDecl;
	bgfx::VertexDecl& srcDecl  = *(bgfx::VertexDecl*)_srcDecl;
	bgfx::vertexConvert(destDecl, _destData, srcDecl, _srcData, _num);
}

BGFX_C_API uint16_t bgfx_weld_vertices(uint16_t* _output, const bgfx_vertex_decl_t* _decl, const void* _data, uint16_t _num, float _epsilon)
{
	bgfx::VertexDecl& decl = *(bgfx::VertexDecl*)_decl;
	return bgfx::weldVertices(_output, decl, _data, _num, _epsilon);
}

BGFX_C_API void bgfx_image_swizzle_bgra8(uint32_t _width, uint32_t _height, uint32_t _pitch, const void* _src, void* _dst)
{
	bgfx::imageSwizzleBgra8(_width, _height, _pitch, _src, _dst);
}

BGFX_C_API void bgfx_image_rgba8_downsample_2x2(uint32_t _width, uint32_t _height, uint32_t _pitch, const void* _src, void* _dst)
{
	bgfx::imageRgba8Downsample2x2(_width, _height, _pitch, _src, _dst);
}

BGFX_C_API uint8_t bgfx_get_supported_renderers(bgfx_renderer_type_t _enum[BGFX_RENDERER_TYPE_COUNT])
{
	return bgfx::getSupportedRenderers( (bgfx::RendererType::Enum*)_enum);
}

BGFX_C_API const char* bgfx_get_renderer_name(bgfx_renderer_type_t _type)
{
	return bgfx::getRendererName(bgfx::RendererType::Enum(_type) );
}

BGFX_C_API bool bgfx_init(bgfx_renderer_type_t _type, uint16_t _vendorId, uint16_t _deviceId, bgfx_callback_interface_t* _callback, bgfx_reallocator_interface_t* _allocator)
{
	static bgfx::CallbackC99 s_callback;
	s_callback.m_interface = _callback;

	static bgfx::AllocatorC99 s_allocator;
	s_allocator.m_interface = _allocator;

	return bgfx::init(bgfx::RendererType::Enum(_type)
		, _vendorId
		, _deviceId
		, NULL == _callback  ? NULL : &s_callback
		, NULL == _allocator ? NULL : &s_allocator
		);
}

BGFX_C_API void bgfx_shutdown()
{
	return bgfx::shutdown();
}

BGFX_C_API void bgfx_reset(uint32_t _width, uint32_t _height, uint32_t _flags)
{
	bgfx::reset(_width, _height, _flags);
}

BGFX_C_API uint32_t bgfx_frame()
{
	return bgfx::frame();
}

BGFX_C_API bgfx_renderer_type_t bgfx_get_renderer_type()
{
	return bgfx_renderer_type_t(bgfx::getRendererType() );
}

BGFX_C_API const bgfx_caps_t* bgfx_get_caps()
{
	return (const bgfx_caps_t*)bgfx::getCaps();
}

BGFX_C_API const bgfx_hmd_t* bgfx_get_hmd()
{
	return (const bgfx_hmd_t*)bgfx::getHMD();
}

BGFX_C_API const bgfx_stats_t* bgfx_get_stats()
{
	return (const bgfx_stats_t*)bgfx::getStats();
}

BGFX_C_API const bgfx_memory_t* bgfx_alloc(uint32_t _size)
{
	return (const bgfx_memory_t*)bgfx::alloc(_size);
}

BGFX_C_API const bgfx_memory_t* bgfx_copy(const void* _data, uint32_t _size)
{
	return (const bgfx_memory_t*)bgfx::copy(_data, _size);
}

BGFX_C_API const bgfx_memory_t* bgfx_make_ref(const void* _data, uint32_t _size)
{
	return (const bgfx_memory_t*)bgfx::makeRef(_data, _size);
}

BGFX_C_API const bgfx_memory_t* bgfx_make_ref_release(const void* _data, uint32_t _size, bgfx_release_fn_t _releaseFn, void* _userData)
{
	return (const bgfx_memory_t*)bgfx::makeRef(_data, _size, _releaseFn, _userData);
}

BGFX_C_API void bgfx_set_debug(uint32_t _debug)
{
	bgfx::setDebug(_debug);
}

BGFX_C_API void bgfx_dbg_text_clear(uint8_t _attr, bool _small)
{
	bgfx::dbgTextClear(_attr, _small);
}

BGFX_C_API void bgfx_dbg_text_printf(uint16_t _x, uint16_t _y, uint8_t _attr, const char* _format, ...)
{
	va_list argList;
	va_start(argList, _format);
	bgfx::dbgTextPrintfVargs(_x, _y, _attr, _format, argList);
	va_end(argList);
}

BGFX_C_API void bgfx_dbg_text_image(uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const void* _data, uint16_t _pitch)
{
	bgfx::dbgTextImage(_x, _y, _width, _height, _data, _pitch);
}

BGFX_C_API bgfx_index_buffer_handle_t bgfx_create_index_buffer(const bgfx_memory_t* _mem, uint16_t _flags)
{
	union { bgfx_index_buffer_handle_t c; bgfx::IndexBufferHandle cpp; } handle;
	handle.cpp = bgfx::createIndexBuffer( (const bgfx::Memory*)_mem, _flags);
	return handle.c;
}

BGFX_C_API void bgfx_destroy_index_buffer(bgfx_index_buffer_handle_t _handle)
{
	union { bgfx_index_buffer_handle_t c; bgfx::IndexBufferHandle cpp; } handle = { _handle };
	bgfx::destroyIndexBuffer(handle.cpp);
}

BGFX_C_API bgfx_vertex_buffer_handle_t bgfx_create_vertex_buffer(const bgfx_memory_t* _mem, const bgfx_vertex_decl_t* _decl, uint16_t _flags)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	union { bgfx_vertex_buffer_handle_t c; bgfx::VertexBufferHandle cpp; } handle;
	handle.cpp = bgfx::createVertexBuffer( (const bgfx::Memory*)_mem, decl, _flags);
	return handle.c;
}

BGFX_C_API void bgfx_destroy_vertex_buffer(bgfx_vertex_buffer_handle_t _handle)
{
	union { bgfx_vertex_buffer_handle_t c; bgfx::VertexBufferHandle cpp; } handle = { _handle };
	bgfx::destroyVertexBuffer(handle.cpp);
}

BGFX_C_API bgfx_dynamic_index_buffer_handle_t bgfx_create_dynamic_index_buffer(uint32_t _num, uint16_t _flags)
{
	union { bgfx_dynamic_index_buffer_handle_t c; bgfx::DynamicIndexBufferHandle cpp; } handle;
	handle.cpp = bgfx::createDynamicIndexBuffer(_num, _flags);
	return handle.c;
}

BGFX_C_API bgfx_dynamic_index_buffer_handle_t bgfx_create_dynamic_index_buffer_mem(const bgfx_memory_t* _mem, uint16_t _flags)
{
	union { bgfx_dynamic_index_buffer_handle_t c; bgfx::DynamicIndexBufferHandle cpp; } handle;
	handle.cpp = bgfx::createDynamicIndexBuffer( (const bgfx::Memory*)_mem, _flags);
	return handle.c;
}

BGFX_C_API void bgfx_update_dynamic_index_buffer(bgfx_dynamic_index_buffer_handle_t _handle, uint32_t _startIndex, const bgfx_memory_t* _mem)
{
	union { bgfx_dynamic_index_buffer_handle_t c; bgfx::DynamicIndexBufferHandle cpp; } handle = { _handle };
	bgfx::updateDynamicIndexBuffer(handle.cpp, _startIndex, (const bgfx::Memory*)_mem);
}

BGFX_C_API void bgfx_destroy_dynamic_index_buffer(bgfx_dynamic_index_buffer_handle_t _handle)
{
	union { bgfx_dynamic_index_buffer_handle_t c; bgfx::DynamicIndexBufferHandle cpp; } handle = { _handle };
	bgfx::destroyDynamicIndexBuffer(handle.cpp);
}

BGFX_C_API bgfx_dynamic_vertex_buffer_handle_t bgfx_create_dynamic_vertex_buffer(uint32_t _num, const bgfx_vertex_decl_t* _decl, uint16_t _flags)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle;
	handle.cpp = bgfx::createDynamicVertexBuffer(_num, decl, _flags);
	return handle.c;
}

BGFX_C_API bgfx_dynamic_vertex_buffer_handle_t bgfx_create_dynamic_vertex_buffer_mem(const bgfx_memory_t* _mem, const bgfx_vertex_decl_t* _decl, uint16_t _flags)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle;
	handle.cpp = bgfx::createDynamicVertexBuffer( (const bgfx::Memory*)_mem, decl, _flags);
	return handle.c;
}

BGFX_C_API void bgfx_update_dynamic_vertex_buffer(bgfx_dynamic_vertex_buffer_handle_t _handle, uint32_t _startVertex, const bgfx_memory_t* _mem)
{
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle = { _handle };
	bgfx::updateDynamicVertexBuffer(handle.cpp, _startVertex, (const bgfx::Memory*)_mem);
}

BGFX_C_API void bgfx_destroy_dynamic_vertex_buffer(bgfx_dynamic_vertex_buffer_handle_t _handle)
{
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle = { _handle };
	bgfx::destroyDynamicVertexBuffer(handle.cpp);
}

BGFX_C_API bool bgfx_check_avail_transient_index_buffer(uint32_t _num)
{
	return bgfx::checkAvailTransientIndexBuffer(_num);
}

BGFX_C_API bool bgfx_check_avail_transient_vertex_buffer(uint32_t _num, const bgfx_vertex_decl_t* _decl)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	return bgfx::checkAvailTransientVertexBuffer(_num, decl);
}

BGFX_C_API bool bgfx_check_avail_instance_data_buffer(uint32_t _num, uint16_t _stride)
{
	return bgfx::checkAvailInstanceDataBuffer(_num, _stride);
}

BGFX_C_API bool bgfx_check_avail_transient_buffers(uint32_t _numVertices, const bgfx_vertex_decl_t* _decl, uint32_t _numIndices)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	return bgfx::checkAvailTransientBuffers(_numVertices, decl, _numIndices);
}

BGFX_C_API void bgfx_alloc_transient_index_buffer(bgfx_transient_index_buffer_t* _tib, uint32_t _num)
{
	bgfx::allocTransientIndexBuffer( (bgfx::TransientIndexBuffer*)_tib, _num);
}

BGFX_C_API void bgfx_alloc_transient_vertex_buffer(bgfx_transient_vertex_buffer_t* _tvb, uint32_t _num, const bgfx_vertex_decl_t* _decl)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	bgfx::allocTransientVertexBuffer( (bgfx::TransientVertexBuffer*)_tvb, _num, decl);
}

BGFX_C_API bool bgfx_alloc_transient_buffers(bgfx_transient_vertex_buffer_t* _tvb, const bgfx_vertex_decl_t* _decl, uint32_t _numVertices, bgfx_transient_index_buffer_t* _tib, uint32_t _numIndices)
{
	const bgfx::VertexDecl& decl = *(const bgfx::VertexDecl*)_decl;
	return bgfx::allocTransientBuffers( (bgfx::TransientVertexBuffer*)_tvb, decl, _numVertices, (bgfx::TransientIndexBuffer*)_tib, _numIndices);
}

BGFX_C_API const bgfx_instance_data_buffer_t* bgfx_alloc_instance_data_buffer(uint32_t _num, uint16_t _stride)
{
	return (bgfx_instance_data_buffer_t*)bgfx::allocInstanceDataBuffer(_num, _stride);
}

BGFX_C_API bgfx_indirect_buffer_handle_t bgfx_create_indirect_buffer(uint32_t _num)
{
	union { bgfx_indirect_buffer_handle_t c; bgfx::IndirectBufferHandle cpp; } handle;
	handle.cpp = bgfx::createIndirectBuffer(_num);
	return handle.c;
}

BGFX_C_API void bgfx_destroy_indirect_buffer(bgfx_indirect_buffer_handle_t _handle)
{
	union { bgfx_indirect_buffer_handle_t c; bgfx::IndirectBufferHandle cpp; } handle = { _handle };
	bgfx::destroyIndirectBuffer(handle.cpp);
}

BGFX_C_API bgfx_shader_handle_t bgfx_create_shader(const bgfx_memory_t* _mem)
{
	union { bgfx_shader_handle_t c; bgfx::ShaderHandle cpp; } handle;
	handle.cpp = bgfx::createShader( (const bgfx::Memory*)_mem);
	return handle.c;
}

BGFX_C_API uint16_t bgfx_get_shader_uniforms(bgfx_shader_handle_t _handle, bgfx_uniform_handle_t* _uniforms, uint16_t _max)
{
	union { bgfx_shader_handle_t c; bgfx::ShaderHandle cpp; } handle = { _handle };
	return bgfx::getShaderUniforms(handle.cpp, (bgfx::UniformHandle*)_uniforms, _max);
}

BGFX_C_API void bgfx_destroy_shader(bgfx_shader_handle_t _handle)
{
	union { bgfx_shader_handle_t c; bgfx::ShaderHandle cpp; } handle = { _handle };
	bgfx::destroyShader(handle.cpp);
}

BGFX_C_API bgfx_program_handle_t bgfx_create_program(bgfx_shader_handle_t _vsh, bgfx_shader_handle_t _fsh, bool _destroyShaders)
{
	union { bgfx_shader_handle_t c; bgfx::ShaderHandle cpp; } vsh = { _vsh };
	union { bgfx_shader_handle_t c; bgfx::ShaderHandle cpp; } fsh = { _fsh };
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle;
	handle.cpp = bgfx::createProgram(vsh.cpp, fsh.cpp, _destroyShaders);
	return handle.c;
}

BGFX_C_API bgfx_program_handle_t bgfx_create_compute_program(bgfx_shader_handle_t _csh, bool _destroyShaders)
{
	union { bgfx_shader_handle_t c; bgfx::ShaderHandle cpp; } csh = { _csh };
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle;
	handle.cpp = bgfx::createProgram(csh.cpp, _destroyShaders);
	return handle.c;
}

BGFX_C_API void bgfx_destroy_program(bgfx_program_handle_t _handle)
{
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle = { _handle };
	bgfx::destroyProgram(handle.cpp);
}

BGFX_C_API void bgfx_calc_texture_size(bgfx_texture_info_t* _info, uint16_t _width, uint16_t _height, uint16_t _depth, bool _cubeMap, uint8_t _numMips, bgfx_texture_format_t _format)
{
	bgfx::TextureInfo& info = *(bgfx::TextureInfo*)_info;
	bgfx::calcTextureSize(info, _width, _height, _depth, _cubeMap, _numMips, bgfx::TextureFormat::Enum(_format) );
}

BGFX_C_API bgfx_texture_handle_t bgfx_create_texture(const bgfx_memory_t* _mem, uint32_t _flags, uint8_t _skip, bgfx_texture_info_t* _info)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle;
	bgfx::TextureInfo* info = (bgfx::TextureInfo*)_info;
	handle.cpp = bgfx::createTexture( (const bgfx::Memory*)_mem, _flags, _skip, info);
	return handle.c;
}

BGFX_C_API bgfx_texture_handle_t bgfx_create_texture_2d(uint16_t _width, uint16_t _height, uint8_t _numMips, bgfx_texture_format_t _format, uint32_t _flags, const bgfx_memory_t* _mem)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle;
	handle.cpp = bgfx::createTexture2D(_width, _height, _numMips, bgfx::TextureFormat::Enum(_format), _flags, (const bgfx::Memory*)_mem);
	return handle.c;
}

BGFX_C_API bgfx_texture_handle_t bgfx_create_texture_2d_scaled(bgfx_backbuffer_ratio_t _ratio, uint8_t _numMips, bgfx_texture_format_t _format, uint32_t _flags)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle;
	handle.cpp = bgfx::createTexture2D(bgfx::BackbufferRatio::Enum(_ratio), _numMips, bgfx::TextureFormat::Enum(_format), _flags);
	return handle.c;
}

BGFX_C_API bgfx_texture_handle_t bgfx_create_texture_3d(uint16_t _width, uint16_t _height, uint16_t _depth, uint8_t _numMips, bgfx_texture_format_t _format, uint32_t _flags, const bgfx_memory_t* _mem)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle;
	handle.cpp = bgfx::createTexture3D(_width, _height, _depth, _numMips, bgfx::TextureFormat::Enum(_format), _flags, (const bgfx::Memory*)_mem);
	return handle.c;
}

BGFX_C_API bgfx_texture_handle_t bgfx_create_texture_cube(uint16_t _size, uint8_t _numMips, bgfx_texture_format_t _format, uint32_t _flags, const bgfx_memory_t* _mem)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle;
	handle.cpp = bgfx::createTextureCube(_size, _numMips, bgfx::TextureFormat::Enum(_format), _flags, (const bgfx::Memory*)_mem);
	return handle.c;
}

BGFX_C_API void bgfx_update_texture_2d(bgfx_texture_handle_t _handle, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const bgfx_memory_t* _mem, uint16_t _pitch)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle = { _handle };
	bgfx::updateTexture2D(handle.cpp, _mip, _x, _y, _width, _height, (const bgfx::Memory*)_mem, _pitch);
}

BGFX_C_API void bgfx_update_texture_3d(bgfx_texture_handle_t _handle, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _z, uint16_t _width, uint16_t _height, uint16_t _depth, const bgfx_memory_t* _mem)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle = { _handle };
	bgfx::updateTexture3D(handle.cpp, _mip, _x, _y, _z, _width, _height, _depth, (const bgfx::Memory*)_mem);
}

BGFX_C_API void bgfx_update_texture_cube(bgfx_texture_handle_t _handle, uint8_t _side, uint8_t _mip, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height, const bgfx_memory_t* _mem, uint16_t _pitch)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle = { _handle };
	bgfx::updateTextureCube(handle.cpp, _side, _mip, _x, _y, _width, _height, (const bgfx::Memory*)_mem, _pitch);
}

BGFX_C_API void bgfx_destroy_texture(bgfx_texture_handle_t _handle)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle = { _handle };
	bgfx::destroyTexture(handle.cpp);
}

BGFX_C_API bgfx_frame_buffer_handle_t bgfx_create_frame_buffer(uint16_t _width, uint16_t _height, bgfx_texture_format_t _format, uint32_t _textureFlags)
{
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle;
	handle.cpp = bgfx::createFrameBuffer(_width, _height, bgfx::TextureFormat::Enum(_format), _textureFlags);
	return handle.c;
}

BGFX_C_API bgfx_frame_buffer_handle_t bgfx_create_frame_buffer_scaled(bgfx_backbuffer_ratio_t _ratio, bgfx_texture_format_t _format, uint32_t _textureFlags)
{
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle;
	handle.cpp = bgfx::createFrameBuffer(bgfx::BackbufferRatio::Enum(_ratio), bgfx::TextureFormat::Enum(_format), _textureFlags);
	return handle.c;
}

BGFX_C_API bgfx_frame_buffer_handle_t bgfx_create_frame_buffer_from_handles(uint8_t _num, bgfx_texture_handle_t* _handles, bool _destroyTextures)
{
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle;
	handle.cpp = bgfx::createFrameBuffer(_num, (bgfx::TextureHandle*)_handles, _destroyTextures);
	return handle.c;
}

BGFX_C_API bgfx_frame_buffer_handle_t bgfx_create_frame_buffer_from_nwh(void* _nwh, uint16_t _width, uint16_t _height, bgfx_texture_format_t _depthFormat)
{
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle;
	handle.cpp = bgfx::createFrameBuffer(_nwh, _width, _height, bgfx::TextureFormat::Enum(_depthFormat) );
	return handle.c;
}

BGFX_C_API void bgfx_destroy_frame_buffer(bgfx_frame_buffer_handle_t _handle)
{
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle = { _handle };
	bgfx::destroyFrameBuffer(handle.cpp);
}

BGFX_C_API bgfx_uniform_handle_t bgfx_create_uniform(const char* _name, bgfx_uniform_type_t _type, uint16_t _num)
{
	union { bgfx_uniform_handle_t c; bgfx::UniformHandle cpp; } handle;
	handle.cpp = bgfx::createUniform(_name, bgfx::UniformType::Enum(_type), _num);
	return handle.c;
}

BGFX_C_API void bgfx_destroy_uniform(bgfx_uniform_handle_t _handle)
{
	union { bgfx_uniform_handle_t c; bgfx::UniformHandle cpp; } handle = { _handle };
	bgfx::destroyUniform(handle.cpp);
}

BGFX_C_API void bgfx_set_clear_color(uint8_t _index, const float _rgba[4])
{
	bgfx::setClearColor(_index, _rgba);
}

BGFX_C_API void bgfx_set_view_name(uint8_t _id, const char* _name)
{
	bgfx::setViewName(_id, _name);
}

BGFX_C_API void bgfx_set_view_rect(uint8_t _id, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height)
{
	bgfx::setViewRect(_id, _x, _y, _width, _height);
}

BGFX_C_API void bgfx_set_view_scissor(uint8_t _id, uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height)
{
	bgfx::setViewScissor(_id, _x, _y, _width, _height);
}

BGFX_C_API void bgfx_set_view_clear(uint8_t _id, uint16_t _flags, uint32_t _rgba, float _depth, uint8_t _stencil)
{
	bgfx::setViewClear(_id, _flags, _rgba, _depth, _stencil);
}

BGFX_C_API void bgfx_set_view_clear_mrt(uint8_t _id, uint16_t _flags, float _depth, uint8_t _stencil, uint8_t _0, uint8_t _1, uint8_t _2, uint8_t _3, uint8_t _4, uint8_t _5, uint8_t _6, uint8_t _7)
{
	bgfx::setViewClear(_id, _flags, _depth, _stencil, _0, _1, _2, _3, _4, _5, _6, _7);
}

BGFX_C_API void bgfx_set_view_seq(uint8_t _id, bool _enabled)
{
	bgfx::setViewSeq(_id, _enabled);
}

BGFX_C_API void bgfx_set_view_frame_buffer(uint8_t _id, bgfx_frame_buffer_handle_t _handle)
{
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle = { _handle };
	bgfx::setViewFrameBuffer(_id, handle.cpp);
}

BGFX_C_API void bgfx_set_view_transform(uint8_t _id, const void* _view, const void* _proj)
{
	bgfx::setViewTransform(_id, _view, _proj);
}

BGFX_C_API void bgfx_set_view_transform_stereo(uint8_t _id, const void* _view, const void* _projL, uint8_t _flags, const void* _projR)
{
	bgfx::setViewTransform(_id, _view, _projL, _flags, _projR);
}

BGFX_C_API void bgfx_set_view_remap(uint8_t _id, uint8_t _num, const void* _remap)
{
	bgfx::setViewRemap(_id, _num, _remap);
}

BGFX_C_API void bgfx_set_marker(const char* _marker)
{
	bgfx::setMarker(_marker);
}

BGFX_C_API void bgfx_set_state(uint64_t _state, uint32_t _rgba)
{
	bgfx::setState(_state, _rgba);
}

BGFX_C_API void bgfx_set_stencil(uint32_t _fstencil, uint32_t _bstencil)
{
	bgfx::setStencil(_fstencil, _bstencil);
}

BGFX_C_API uint16_t bgfx_set_scissor(uint16_t _x, uint16_t _y, uint16_t _width, uint16_t _height)
{
	return bgfx::setScissor(_x, _y, _width, _height);
}

BGFX_C_API void bgfx_set_scissor_cached(uint16_t _cache)
{
	bgfx::setScissor(_cache);
}

BGFX_C_API uint32_t bgfx_set_transform(const void* _mtx, uint16_t _num)
{
	return bgfx::setTransform(_mtx, _num);
}

BGFX_C_API uint32_t bgfx_alloc_transform(bgfx_transform_t* _transform, uint16_t _num)
{
	return bgfx::allocTransform( (bgfx::Transform*)_transform, _num);
}

BGFX_C_API void bgfx_set_transform_cached(uint32_t _cache, uint16_t _num)
{
	bgfx::setTransform(_cache, _num);
}

BGFX_C_API void bgfx_set_uniform(bgfx_uniform_handle_t _handle, const void* _value, uint16_t _num)
{
	union { bgfx_uniform_handle_t c; bgfx::UniformHandle cpp; } handle = { _handle };
	bgfx::setUniform(handle.cpp, _value, _num);
}

BGFX_C_API void bgfx_set_index_buffer(bgfx_index_buffer_handle_t _handle, uint32_t _firstIndex, uint32_t _numIndices)
{
	union { bgfx_index_buffer_handle_t c; bgfx::IndexBufferHandle cpp; } handle = { _handle };
	bgfx::setIndexBuffer(handle.cpp, _firstIndex, _numIndices);
}

BGFX_C_API void bgfx_set_dynamic_index_buffer(bgfx_dynamic_index_buffer_handle_t _handle, uint32_t _firstIndex, uint32_t _numIndices)
{
	union { bgfx_dynamic_index_buffer_handle_t c; bgfx::DynamicIndexBufferHandle cpp; } handle = { _handle };
	bgfx::setIndexBuffer(handle.cpp, _firstIndex, _numIndices);
}

BGFX_C_API void bgfx_set_transient_index_buffer(const bgfx_transient_index_buffer_t* _tib, uint32_t _firstIndex, uint32_t _numIndices)
{
	bgfx::setIndexBuffer( (const bgfx::TransientIndexBuffer*)_tib, _firstIndex, _numIndices);
}

BGFX_C_API void bgfx_set_vertex_buffer(bgfx_vertex_buffer_handle_t _handle, uint32_t _startVertex, uint32_t _numVertices)
{
	union { bgfx_vertex_buffer_handle_t c; bgfx::VertexBufferHandle cpp; } handle = { _handle };
	bgfx::setVertexBuffer(handle.cpp, _startVertex, _numVertices);
}

BGFX_C_API void bgfx_set_dynamic_vertex_buffer(bgfx_dynamic_vertex_buffer_handle_t _handle, uint32_t _numVertices)
{
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle = { _handle };
	bgfx::setVertexBuffer(handle.cpp, _numVertices);
}

BGFX_C_API void bgfx_set_transient_vertex_buffer(const bgfx_transient_vertex_buffer_t* _tvb, uint32_t _startVertex, uint32_t _numVertices)
{
	bgfx::setVertexBuffer( (const bgfx::TransientVertexBuffer*)_tvb, _startVertex, _numVertices);
}

BGFX_C_API void bgfx_set_instance_data_buffer(const bgfx_instance_data_buffer_t* _idb, uint32_t _num)
{
	bgfx::setInstanceDataBuffer( (const bgfx::InstanceDataBuffer*)_idb, _num);
}

BGFX_C_API void bgfx_set_instance_data_from_vertex_buffer(bgfx_vertex_buffer_handle_t _handle, uint32_t _startVertex, uint32_t _num)
{
	union { bgfx_vertex_buffer_handle_t c; bgfx::VertexBufferHandle cpp; } handle = { _handle };
	bgfx::setInstanceDataBuffer(handle.cpp, _startVertex, _num);
}

BGFX_C_API void bgfx_set_instance_data_from_dynamic_vertex_buffer(bgfx_dynamic_vertex_buffer_handle_t _handle, uint32_t _startVertex, uint32_t _num)
{
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle = { _handle };
	bgfx::setInstanceDataBuffer(handle.cpp, _startVertex, _num);
}

BGFX_C_API void bgfx_set_texture(uint8_t _stage, bgfx_uniform_handle_t _sampler, bgfx_texture_handle_t _handle, uint32_t _flags)
{
	union { bgfx_uniform_handle_t c; bgfx::UniformHandle cpp; } sampler = { _sampler };
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle  = { _handle  };
	bgfx::setTexture(_stage, sampler.cpp, handle.cpp, _flags);
}

BGFX_C_API void bgfx_set_texture_from_frame_buffer(uint8_t _stage, bgfx_uniform_handle_t _sampler, bgfx_frame_buffer_handle_t _handle, uint8_t _attachment, uint32_t _flags)
{
	union { bgfx_uniform_handle_t c;      bgfx::UniformHandle cpp;     } sampler = { _sampler };
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle  = { _handle };
	bgfx::setTexture(_stage, sampler.cpp, handle.cpp, _attachment, _flags);
}

BGFX_C_API uint32_t bgfx_touch(uint8_t _id)
{
	return bgfx::touch(_id);
}

BGFX_C_API uint32_t bgfx_submit(uint8_t _id, bgfx_program_handle_t _handle, int32_t _depth)
{
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle = { _handle };
	return bgfx::submit(_id, handle.cpp, _depth);
}

BGFX_C_API uint32_t bgfx_submit_indirect(uint8_t _id, bgfx_program_handle_t _handle, bgfx_indirect_buffer_handle_t _indirectHandle, uint16_t _start, uint16_t _num, int32_t _depth)
{
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle = { _handle };
	union { bgfx_indirect_buffer_handle_t c; bgfx::IndirectBufferHandle cpp; } indirectHandle = { _indirectHandle };
	return bgfx::submit(_id, handle.cpp, indirectHandle.cpp, _start, _num, _depth);
}

BGFX_C_API void bgfx_set_image(uint8_t _stage, bgfx_uniform_handle_t _sampler, bgfx_texture_handle_t _handle, uint8_t _mip, bgfx_access_t _access, bgfx_texture_format_t _format)
{
	union { bgfx_uniform_handle_t c; bgfx::UniformHandle cpp; } sampler = { _sampler };
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } handle  = { _handle  };
	bgfx::setImage(_stage, sampler.cpp, handle.cpp, _mip, bgfx::Access::Enum(_access), bgfx::TextureFormat::Enum(_format) );
}

BGFX_C_API void bgfx_set_image_from_frame_buffer(uint8_t _stage, bgfx_uniform_handle_t _sampler, bgfx_frame_buffer_handle_t _handle, uint8_t _attachment, bgfx_access_t _access, bgfx_texture_format_t _format)
{
	union { bgfx_uniform_handle_t c;      bgfx::UniformHandle cpp;     } sampler = { _sampler };
	union { bgfx_frame_buffer_handle_t c; bgfx::FrameBufferHandle cpp; } handle  = { _handle };
	bgfx::setImage(_stage, sampler.cpp, handle.cpp, _attachment, bgfx::Access::Enum(_access), bgfx::TextureFormat::Enum(_format) );
}

BGFX_C_API void bgfx_set_compute_index_buffer(uint8_t _stage, bgfx_index_buffer_handle_t _handle, bgfx_access_t _access)
{
	union { bgfx_index_buffer_handle_t c; bgfx::IndexBufferHandle cpp; } handle = { _handle };
	bgfx::setBuffer(_stage, handle.cpp, bgfx::Access::Enum(_access) );
}

BGFX_C_API void bgfx_set_compute_vertex_buffer(uint8_t _stage, bgfx_vertex_buffer_handle_t _handle, bgfx_access_t _access)
{
	union { bgfx_vertex_buffer_handle_t c; bgfx::VertexBufferHandle cpp; } handle = { _handle };
	bgfx::setBuffer(_stage, handle.cpp, bgfx::Access::Enum(_access) );
}

BGFX_C_API void bgfx_set_compute_dynamic_index_buffer(uint8_t _stage, bgfx_dynamic_index_buffer_handle_t _handle, bgfx_access_t _access)
{
	union { bgfx_dynamic_index_buffer_handle_t c; bgfx::DynamicIndexBufferHandle cpp; } handle = { _handle };
	bgfx::setBuffer(_stage, handle.cpp, bgfx::Access::Enum(_access) );
}

BGFX_C_API void bgfx_set_compute_dynamic_vertex_buffer(uint8_t _stage, bgfx_dynamic_vertex_buffer_handle_t _handle, bgfx_access_t _access)
{
	union { bgfx_dynamic_vertex_buffer_handle_t c; bgfx::DynamicVertexBufferHandle cpp; } handle = { _handle };
	bgfx::setBuffer(_stage, handle.cpp, bgfx::Access::Enum(_access) );
}

BGFX_C_API void bgfx_set_compute_indirect_buffer(uint8_t _stage, bgfx_indirect_buffer_handle_t _handle, bgfx_access_t _access)
{
	union { bgfx_indirect_buffer_handle_t c; bgfx::IndirectBufferHandle cpp; } handle = { _handle };
	bgfx::setBuffer(_stage, handle.cpp, bgfx::Access::Enum(_access) );
}

BGFX_C_API uint32_t bgfx_dispatch(uint8_t _id, bgfx_program_handle_t _handle, uint16_t _numX, uint16_t _numY, uint16_t _numZ, uint8_t _flags)
{
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle = { _handle };
	return bgfx::dispatch(_id, handle.cpp, _numX, _numY, _numZ, _flags);
}

BGFX_C_API uint32_t bgfx_dispatch_indirect(uint8_t _id, bgfx_program_handle_t _handle, bgfx_indirect_buffer_handle_t _indirectHandle, uint16_t _start, uint16_t _num, uint8_t _flags)
{
	union { bgfx_program_handle_t c; bgfx::ProgramHandle cpp; } handle = { _handle };
	union { bgfx_indirect_buffer_handle_t c; bgfx::IndirectBufferHandle cpp; } indirectHandle = { _indirectHandle };
	return bgfx::dispatch(_id, handle.cpp, indirectHandle.cpp, _start, _num, _flags);
}

BGFX_C_API void bgfx_discard()
{
	bgfx::discard();
}

BGFX_C_API void bgfx_save_screen_shot(const char* _filePath)
{
	bgfx::saveScreenShot(_filePath);
}

BGFX_C_API bgfx_render_frame_t bgfx_render_frame()
{
	return bgfx_render_frame_t(bgfx::renderFrame() );
}

BGFX_C_API void bgfx_set_platform_data(bgfx_platform_data_t* _pd)
{
	bgfx::setPlatformData(*(bgfx::PlatformData*)_pd);
}
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
#	include "renderer_gl.h"

#	if BGFX_USE_EGL

#		if BX_PLATFORM_RPI
#			include <bcm_host.h>
#		endif // BX_PLATFORM_RPI

namespace bgfx { namespace gl
{
#ifndef EGL_CONTEXT_FLAG_NO_ERROR_BIT_KHR
#	define EGL_CONTEXT_FLAG_NO_ERROR_BIT_KHR 0x00000008
#endif // EGL_CONTEXT_FLAG_NO_ERROR_BIT_KHR

#if BGFX_USE_GL_DYNAMIC_LIB

	typedef void (*EGLPROC)(void);

	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLCHOOSECONFIGPROC)(EGLDisplay dpy, const EGLint *attrib_list,	EGLConfig *configs, EGLint config_size,	EGLint *num_config);
	typedef EGLContext  (EGLAPIENTRY* PFNEGLCREATECONTEXTPROC)(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);
	typedef EGLSurface  (EGLAPIENTRY* PFNEGLCREATEWINDOWSURFACEPROC)(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list);
	typedef EGLint      (EGLAPIENTRY* PFNEGLGETERRORPROC)(void);
	typedef EGLDisplay  (EGLAPIENTRY* PFNEGLGETDISPLAYPROC)(EGLNativeDisplayType display_id);
	typedef EGLPROC     (EGLAPIENTRY* PFNEGLGETPROCADDRESSPROC)(const char *procname);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLINITIALIZEPROC)(EGLDisplay dpy, EGLint *major, EGLint *minor);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLMAKECURRENTPROC)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLDESTROYCONTEXTPROC)(EGLDisplay dpy, EGLContext ctx);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLDESTROYSURFACEPROC)(EGLDisplay dpy, EGLSurface surface);
	typedef const char* (EGLAPIENTRY* PGNEGLQUERYSTRINGPROC)(EGLDisplay dpy, EGLint name);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLSWAPBUFFERSPROC)(EGLDisplay dpy, EGLSurface surface);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLSWAPINTERVALPROC)(EGLDisplay dpy, EGLint interval);
	typedef EGLBoolean  (EGLAPIENTRY* PFNEGLTERMINATEPROC)(EGLDisplay dpy);

#define EGL_IMPORT \
			EGL_IMPORT_FUNC(PFNEGLCHOOSECONFIGPROC,        eglChooseConfig); \
			EGL_IMPORT_FUNC(PFNEGLCREATECONTEXTPROC,       eglCreateContext); \
			EGL_IMPORT_FUNC(PFNEGLCREATEWINDOWSURFACEPROC, eglCreateWindowSurface); \
			EGL_IMPORT_FUNC(PFNEGLGETDISPLAYPROC,          eglGetDisplay); \
			EGL_IMPORT_FUNC(PFNEGLGETERRORPROC,            eglGetError); \
			EGL_IMPORT_FUNC(PFNEGLGETPROCADDRESSPROC,      eglGetProcAddress); \
			EGL_IMPORT_FUNC(PFNEGLDESTROYCONTEXTPROC,      eglDestroyContext); \
			EGL_IMPORT_FUNC(PFNEGLDESTROYSURFACEPROC,      eglDestroySurface); \
			EGL_IMPORT_FUNC(PFNEGLINITIALIZEPROC,          eglInitialize); \
			EGL_IMPORT_FUNC(PFNEGLMAKECURRENTPROC,         eglMakeCurrent); \
			EGL_IMPORT_FUNC(PGNEGLQUERYSTRINGPROC,         eglQueryString); \
			EGL_IMPORT_FUNC(PFNEGLSWAPBUFFERSPROC,         eglSwapBuffers); \
			EGL_IMPORT_FUNC(PFNEGLSWAPINTERVALPROC,        eglSwapInterval); \
			EGL_IMPORT_FUNC(PFNEGLTERMINATEPROC,           eglTerminate);

#define EGL_IMPORT_FUNC(_proto, _func) _proto _func
EGL_IMPORT
#undef EGL_IMPORT_FUNC

	void* eglOpen()
	{
		void* handle = bx::dlopen("libEGL." BX_DL_EXT);
		BGFX_FATAL(NULL != handle, Fatal::UnableToInitialize, "Failed to load libEGL dynamic library.");

#define EGL_IMPORT_FUNC(_proto, _func) \
			_func = (_proto)bx::dlsym(handle, #_func); \
			BX_TRACE("%p " #_func, _func); \
			BGFX_FATAL(NULL != _func, Fatal::UnableToInitialize, "Failed get " #_func ".")
EGL_IMPORT
#undef EGL_IMPORT_FUNC

		return handle;
	}

	void eglClose(void* _handle)
	{
		bx::dlclose(_handle);

#define EGL_IMPORT_FUNC(_proto, _func) _func = NULL
EGL_IMPORT
#undef EGL_IMPORT_FUNC
	}

#else

	void* eglOpen()
	{
		return NULL;
	}

	void eglClose(void* /*_handle*/)
	{
	}
#endif // BGFX_USE_GL_DYNAMIC_LIB

#	define GL_IMPORT(_optional, _proto, _func, _import) _proto _func = NULL
#	include "glimports.h"

	static EGLint s_contextAttrs[16];

	struct SwapChainGL
	{
		SwapChainGL(EGLDisplay _display, EGLConfig _config, EGLContext _context, EGLNativeWindowType _nwh)
			: m_nwh(_nwh)
			, m_display(_display)
		{
			m_surface = eglCreateWindowSurface(m_display, _config, _nwh, NULL);
			BGFX_FATAL(m_surface != EGL_NO_SURFACE, Fatal::UnableToInitialize, "Failed to create surface.");

			m_context = eglCreateContext(m_display, _config, _context, s_contextAttrs);
			BX_CHECK(NULL != m_context, "Create swap chain failed: %x", eglGetError() );

			makeCurrent();
			GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 0.0f) );
			GL_CHECK(glClear(GL_COLOR_BUFFER_BIT) );
			swapBuffers();
			GL_CHECK(glClear(GL_COLOR_BUFFER_BIT) );
			swapBuffers();
		}

		~SwapChainGL()
		{
			eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroyContext(m_display, m_context);
			eglDestroySurface(m_display, m_surface);
		}

		void makeCurrent()
		{
			eglMakeCurrent(m_display, m_surface, m_surface, m_context);
		}

		void swapBuffers()
		{
			eglSwapBuffers(m_display, m_surface);
		}

		EGLNativeWindowType m_nwh;
		EGLContext m_context;
		EGLDisplay m_display;
		EGLSurface m_surface;
	};

#	if BX_PLATFORM_RPI
	static EGL_DISPMANX_WINDOW_T s_dispmanWindow;

	void x11SetDisplayWindow(::Display* _display, ::Window _window)
	{
		// Noop for now...
		BX_UNUSED(_display, _window);
	}
#	endif // BX_PLATFORM_RPI

	void GlContext::create(uint32_t _width, uint32_t _height)
	{
#	if BX_PLATFORM_RPI
		bcm_host_init();
#	endif // BX_PLATFORM_RPI

		m_eglLibrary = eglOpen();

		if (NULL == g_platformData.context)
		{
			BX_UNUSED(_width, _height);
			EGLNativeDisplayType ndt = (EGLNativeDisplayType)g_platformData.ndt;
			EGLNativeWindowType  nwh = (EGLNativeWindowType )g_platformData.nwh;

#	if BX_PLATFORM_WINDOWS
			if (NULL == g_platformData.ndt)
			{
				ndt = GetDC( (HWND)g_platformData.nwh);
			}
#	endif // BX_PLATFORM_WINDOWS

			m_display = eglGetDisplay(ndt);
			BGFX_FATAL(m_display != EGL_NO_DISPLAY, Fatal::UnableToInitialize, "Failed to create display %p", m_display);

			EGLint major = 0;
			EGLint minor = 0;
			EGLBoolean success = eglInitialize(m_display, &major, &minor);
			BGFX_FATAL(success && major >= 1 && minor >= 3, Fatal::UnableToInitialize, "Failed to initialize %d.%d", major, minor);

			BX_TRACE("EGL info:");
			const char* clientApis = eglQueryString(m_display, EGL_CLIENT_APIS);
			BX_TRACE("   APIs: %s", clientApis); BX_UNUSED(clientApis);

			const char* vendor = eglQueryString(m_display, EGL_VENDOR);
			BX_TRACE(" Vendor: %s", vendor); BX_UNUSED(vendor);

			const char* version = eglQueryString(m_display, EGL_VERSION);
			BX_TRACE("Version: %s", version); BX_UNUSED(version);

			const char* extensions = eglQueryString(m_display, EGL_EXTENSIONS);
			BX_TRACE("Supported EGL extensions:");
			dumpExtensions(extensions);

			EGLint attrs[] =
			{
				EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,

#	if BX_PLATFORM_ANDROID
				EGL_DEPTH_SIZE, 16,
#	else
				EGL_DEPTH_SIZE, 24,
#	endif // BX_PLATFORM_
				EGL_STENCIL_SIZE, 8,

				EGL_NONE
			};

			EGLint numConfig = 0;
			success = eglChooseConfig(m_display, attrs, &m_config, 1, &numConfig);
			BGFX_FATAL(success, Fatal::UnableToInitialize, "eglChooseConfig");

#	if BX_PLATFORM_ANDROID

			EGLint format;
			eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &format);
			ANativeWindow_setBuffersGeometry( (ANativeWindow*)g_platformData.nwh, _width, _height, format);

#	elif BX_PLATFORM_RPI
			DISPMANX_DISPLAY_HANDLE_T dispmanDisplay = vc_dispmanx_display_open(0);
			DISPMANX_UPDATE_HANDLE_T  dispmanUpdate  = vc_dispmanx_update_start(0);

			VC_RECT_T dstRect = { 0, 0, _width,        _height       };
			VC_RECT_T srcRect = { 0, 0, _width  << 16, _height << 16 };

			DISPMANX_ELEMENT_HANDLE_T dispmanElement = vc_dispmanx_element_add(dispmanUpdate
				, dispmanDisplay
				, 0
				, &dstRect
				, 0
				, &srcRect
				, DISPMANX_PROTECTION_NONE
				, NULL
				, NULL
				, DISPMANX_NO_ROTATE
				);

			s_dispmanWindow.element = dispmanElement;
			s_dispmanWindow.width   = _width;
			s_dispmanWindow.height  = _height;
			nwh = &s_dispmanWindow;

			vc_dispmanx_update_submit_sync(dispmanUpdate);
#	endif // BX_PLATFORM_ANDROID

			m_surface = eglCreateWindowSurface(m_display, m_config, nwh, NULL);
			BGFX_FATAL(m_surface != EGL_NO_SURFACE, Fatal::UnableToInitialize, "Failed to create surface.");

			const bool hasEglKhrCreateContext = !!bx::findIdentifierMatch(extensions, "EGL_KHR_create_context");
			const bool hasEglKhrNoError       = !!bx::findIdentifierMatch(extensions, "EGL_KHR_create_context_no_error");

			for (uint32_t ii = 0; ii < 2; ++ii)
			{
				bx::StaticMemoryBlockWriter writer(s_contextAttrs, sizeof(s_contextAttrs) );

				EGLint flags = 0;

				if (hasEglKhrCreateContext)
				{
					bx::write(&writer, EGLint(EGL_CONTEXT_MAJOR_VERSION_KHR) );
					bx::write(&writer, EGLint(BGFX_CONFIG_RENDERER_OPENGLES / 10) );

					bx::write(&writer, EGLint(EGL_CONTEXT_MINOR_VERSION_KHR) );
					bx::write(&writer, EGLint(BGFX_CONFIG_RENDERER_OPENGLES % 10) );

					flags |= BGFX_CONFIG_DEBUG && hasEglKhrNoError ? 0
						| EGL_CONTEXT_FLAG_NO_ERROR_BIT_KHR
						: 0
						;

					if (0 == ii)
					{
						flags |= BGFX_CONFIG_DEBUG ? 0
							| EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR
//							| EGL_OPENGL_ES3_BIT_KHR
							: 0
							;

						bx::write(&writer, EGLint(EGL_CONTEXT_FLAGS_KHR) );
						bx::write(&writer, flags);
					}
				}
				else
				{
					bx::write(&writer, EGLint(EGL_CONTEXT_CLIENT_VERSION) );
					bx::write(&writer, 2);
				}

				bx::write(&writer, EGLint(EGL_NONE) );

				m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, s_contextAttrs);
				if (NULL != m_context)
				{
					break;
				}

				BX_TRACE("Failed to create EGL context with EGL_CONTEXT_FLAGS_KHR (%08x).", flags);
			}

			BGFX_FATAL(m_context != EGL_NO_CONTEXT, Fatal::UnableToInitialize, "Failed to create context.");

			success = eglMakeCurrent(m_display, m_surface, m_surface, m_context);
			BGFX_FATAL(success, Fatal::UnableToInitialize, "Failed to set context.");
			m_current = NULL;

			eglSwapInterval(m_display, 0);
		}

		import();
	}

	void GlContext::destroy()
	{
		if (NULL != m_display)
		{
			eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroyContext(m_display, m_context);
			eglDestroySurface(m_display, m_surface);
			eglTerminate(m_display);
			m_context = NULL;
		}

		eglClose(m_eglLibrary);

#	if BX_PLATFORM_RPI
		bcm_host_deinit();
#	endif // BX_PLATFORM_RPI
	}

	void GlContext::resize(uint32_t _width, uint32_t _height, uint32_t _flags)
	{
#	if BX_PLATFORM_ANDROID
		if (NULL != m_display)
		{
			EGLint format;
			eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &format);
			ANativeWindow_setBuffersGeometry( (ANativeWindow*)g_platformData.nwh, _width, _height, format);
		}
#	elif BX_PLATFORM_EMSCRIPTEN
		emscripten_set_canvas_size(_width, _height);
#	else
		BX_UNUSED(_width, _height);
#	endif // BX_PLATFORM_*

		if (NULL != m_display)
		{
			bool vsync = !!(_flags&BGFX_RESET_VSYNC);
			eglSwapInterval(m_display, vsync ? 1 : 0);
		}
	}

	uint64_t GlContext::getCaps() const
	{
		return BX_ENABLED(0
						| BX_PLATFORM_LINUX
						| BX_PLATFORM_WINDOWS
						)
			? BGFX_CAPS_SWAP_CHAIN
			: 0
			;
	}

	SwapChainGL* GlContext::createSwapChain(void* _nwh)
	{
		return BX_NEW(g_allocator, SwapChainGL)(m_display, m_config, m_context, (EGLNativeWindowType)_nwh);
	}

	void GlContext::destroySwapChain(SwapChainGL* _swapChain)
	{
		BX_DELETE(g_allocator, _swapChain);
	}

	void GlContext::swap(SwapChainGL* _swapChain)
	{
		makeCurrent(_swapChain);

		if (NULL == _swapChain)
		{
			if (NULL != m_display)
			{
				eglSwapBuffers(m_display, m_surface);
			}
		}
		else
		{
			_swapChain->swapBuffers();
		}
	}

	void GlContext::makeCurrent(SwapChainGL* _swapChain)
	{
		if (m_current != _swapChain)
		{
			m_current = _swapChain;

			if (NULL == _swapChain)
			{
				if (NULL != m_display)
				{
					eglMakeCurrent(m_display, m_surface, m_surface, m_context);
				}
			}
			else
			{
				_swapChain->makeCurrent();
			}
		}
	}

	void GlContext::import()
	{
		BX_TRACE("Import:");
#	if BX_PLATFORM_WINDOWS || BX_PLATFORM_LINUX
		void* glesv2 = bx::dlopen("libGLESv2." BX_DL_EXT);
#		define GL_EXTENSION(_optional, _proto, _func, _import) \
					{ \
						if (NULL == _func) \
						{ \
							_func = (_proto)bx::dlsym(glesv2, #_import); \
							BX_TRACE("\t%p " #_func " (" #_import ")", _func); \
							BGFX_FATAL(_optional || NULL != _func, Fatal::UnableToInitialize, "Failed to create OpenGLES context. eglGetProcAddress(\"%s\")", #_import); \
						} \
					}
#	else
#		define GL_EXTENSION(_optional, _proto, _func, _import) \
					{ \
						if (NULL == _func) \
						{ \
							_func = (_proto)eglGetProcAddress(#_import); \
							BX_TRACE("\t%p " #_func " (" #_import ")", _func); \
							BGFX_FATAL(_optional || NULL != _func, Fatal::UnableToInitialize, "Failed to create OpenGLES context. eglGetProcAddress(\"%s\")", #_import); \
						} \
					}
#	endif // BX_PLATFORM_
#	include "glimports.h"
	}

} /* namespace gl */ } // namespace bgfx

#	endif // BGFX_USE_EGL
#endif // (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if (BX_PLATFORM_FREEBSD || BX_PLATFORM_LINUX) && (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
#	include "renderer_gl.h"

#	if BGFX_USE_GLX
#		define GLX_GLXEXT_PROTOTYPES
#		include <glx/glxext.h>

namespace bgfx { namespace gl
{
	typedef int (*PFNGLXSWAPINTERVALMESAPROC)(uint32_t _interval);

	PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB;
	PFNGLXSWAPINTERVALEXTPROC         glXSwapIntervalEXT;
	PFNGLXSWAPINTERVALMESAPROC        glXSwapIntervalMESA;
	PFNGLXSWAPINTERVALSGIPROC         glXSwapIntervalSGI;

#	define GL_IMPORT(_optional, _proto, _func, _import) _proto _func
#	include "glimports.h"

	struct SwapChainGL
	{
		SwapChainGL(::Window _window, XVisualInfo* _visualInfo, GLXContext _context)
			: m_window(_window)
		{
			m_context = glXCreateContext( (::Display*)g_platformData.ndt, _visualInfo, _context, GL_TRUE);
		}

		~SwapChainGL()
		{
			glXMakeCurrent( (::Display*)g_platformData.ndt, 0, 0);
			glXDestroyContext( (::Display*)g_platformData.ndt, m_context);
		}

		void makeCurrent()
		{
			glXMakeCurrent( (::Display*)g_platformData.ndt, m_window, m_context);
		}

		void swapBuffers()
		{
			glXSwapBuffers( (::Display*)g_platformData.ndt, m_window);
		}

		Window m_window;
		GLXContext m_context;
	};

	void GlContext::create(uint32_t _width, uint32_t _height)
	{
		BX_UNUSED(_width, _height);

		m_context = (GLXContext)g_platformData.context;

		if (NULL == g_platformData.context)
		{
			XLockDisplay( (::Display*)g_platformData.ndt);

			int major, minor;
			bool version = glXQueryVersion( (::Display*)g_platformData.ndt, &major, &minor);
			BGFX_FATAL(version, Fatal::UnableToInitialize, "Failed to query GLX version");
			BGFX_FATAL( (major == 1 && minor >= 2) || major > 1
					, Fatal::UnableToInitialize
					, "GLX version is not >=1.2 (%d.%d)."
					, major
					, minor
					);

			int32_t screen = DefaultScreen( (::Display*)g_platformData.ndt);

			const char* extensions = glXQueryExtensionsString( (::Display*)g_platformData.ndt, screen);
			BX_TRACE("GLX extensions:");
			dumpExtensions(extensions);

			const int attrsGlx[] =
			{
				GLX_RENDER_TYPE, GLX_RGBA_BIT,
				GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
				GLX_DOUBLEBUFFER, true,
				GLX_RED_SIZE, 8,
				GLX_BLUE_SIZE, 8,
				GLX_GREEN_SIZE, 8,
				//			GLX_ALPHA_SIZE, 8,
				GLX_DEPTH_SIZE, 24,
				GLX_STENCIL_SIZE, 8,
				0,
			};

			// Find suitable config
			GLXFBConfig bestConfig = NULL;

			int numConfigs;
			GLXFBConfig* configs = glXChooseFBConfig( (::Display*)g_platformData.ndt, screen, attrsGlx, &numConfigs);

			BX_TRACE("glX num configs %d", numConfigs);

			for (int ii = 0; ii < numConfigs; ++ii)
			{
				m_visualInfo = glXGetVisualFromFBConfig( (::Display*)g_platformData.ndt, configs[ii]);
				if (NULL != m_visualInfo)
				{
					BX_TRACE("---");
					bool valid = true;
					for (uint32_t attr = 6; attr < BX_COUNTOF(attrsGlx)-1 && attrsGlx[attr] != None; attr += 2)
					{
						int value;
						glXGetFBConfigAttrib( (::Display*)g_platformData.ndt, configs[ii], attrsGlx[attr], &value);
						BX_TRACE("glX %d/%d %2d: %4x, %8x (%8x%s)"
								, ii
								, numConfigs
								, attr/2
								, attrsGlx[attr]
								, value
								, attrsGlx[attr + 1]
								, value < attrsGlx[attr + 1] ? " *" : ""
								);

						if (value < attrsGlx[attr + 1])
						{
							valid = false;
#if !BGFX_CONFIG_DEBUG
							break;
#endif // BGFX_CONFIG_DEBUG
						}
					}

					if (valid)
					{
						bestConfig = configs[ii];
						BX_TRACE("Best config %d.", ii);
						break;
					}
				}

				XFree(m_visualInfo);
				m_visualInfo = NULL;
			}

			XFree(configs);
			BGFX_FATAL(m_visualInfo, Fatal::UnableToInitialize, "Failed to find a suitable X11 display configuration.");

			BX_TRACE("Create GL 2.1 context.");
			m_context = glXCreateContext( (::Display*)g_platformData.ndt, m_visualInfo, 0, GL_TRUE);
			BGFX_FATAL(NULL != m_context, Fatal::UnableToInitialize, "Failed to create GL 2.1 context.");

#if BGFX_CONFIG_RENDERER_OPENGL >= 31
			glXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress( (const GLubyte*)"glXCreateContextAttribsARB");

			if (NULL != glXCreateContextAttribsARB)
			{
				BX_TRACE("Create GL 3.1 context.");
				const int contextAttrs[] =
				{
					GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
					GLX_CONTEXT_MINOR_VERSION_ARB, 1,
					GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
					0,
				};

				GLXContext context = glXCreateContextAttribsARB( (::Display*)g_platformData.ndt, bestConfig, 0, true, contextAttrs);

				if (NULL != context)
				{
					glXDestroyContext( (::Display*)g_platformData.ndt, m_context);
					m_context = context;
				}
			}
#else
			BX_UNUSED(bestConfig);
#endif // BGFX_CONFIG_RENDERER_OPENGL >= 31

			XUnlockDisplay( (::Display*)g_platformData.ndt);
		}

		import();

		glXMakeCurrent( (::Display*)g_platformData.ndt, (::Window)g_platformData.nwh, m_context);
		m_current = NULL;

		glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddress( (const GLubyte*)"glXSwapIntervalEXT");
		if (NULL != glXSwapIntervalEXT)
		{
			BX_TRACE("Using glXSwapIntervalEXT.");
			glXSwapIntervalEXT( (::Display*)g_platformData.ndt, (::Window)g_platformData.nwh, 0);
		}
		else
		{
			glXSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC)glXGetProcAddress( (const GLubyte*)"glXSwapIntervalMESA");
			if (NULL != glXSwapIntervalMESA)
			{
				BX_TRACE("Using glXSwapIntervalMESA.");
				glXSwapIntervalMESA(0);
			}
			else
			{
				glXSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddress( (const GLubyte*)"glXSwapIntervalSGI");
				if (NULL != glXSwapIntervalSGI)
				{
					BX_TRACE("Using glXSwapIntervalSGI.");
					glXSwapIntervalSGI(0);
				}
			}
		}

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers( (::Display*)g_platformData.ndt, (::Window)g_platformData.nwh);
	}

	void GlContext::destroy()
	{
		glXMakeCurrent( (::Display*)g_platformData.ndt, 0, 0);
		if (NULL == g_platformData.context)
		{
			glXDestroyContext( (::Display*)g_platformData.ndt, m_context);
			XFree(m_visualInfo);
		}
		m_context    = NULL;
		m_visualInfo = NULL;
	}

	void GlContext::resize(uint32_t /*_width*/, uint32_t /*_height*/, uint32_t _flags)
	{
		bool vsync = !!(_flags&BGFX_RESET_VSYNC);
		int32_t interval = vsync ? 1 : 0;

		if (NULL != glXSwapIntervalEXT)
		{
			glXSwapIntervalEXT( (::Display*)g_platformData.ndt, (::Window)g_platformData.nwh, interval);
		}
		else if (NULL != glXSwapIntervalMESA)
		{
			glXSwapIntervalMESA(interval);
		}
		else if (NULL != glXSwapIntervalSGI)
		{
			glXSwapIntervalSGI(interval);
		}
	}

	uint64_t GlContext::getCaps() const
	{
		return BGFX_CAPS_SWAP_CHAIN;
	}

	SwapChainGL* GlContext::createSwapChain(void* _nwh)
	{
		return BX_NEW(g_allocator, SwapChainGL)( (::Window)_nwh, m_visualInfo, m_context);
	}

	void GlContext::destroySwapChain(SwapChainGL* _swapChain)
	{
		BX_DELETE(g_allocator, _swapChain);
	}

	void GlContext::swap(SwapChainGL* _swapChain)
	{
		makeCurrent(_swapChain);

		if (NULL == _swapChain)
		{
			glXSwapBuffers( (::Display*)g_platformData.ndt, (::Window)g_platformData.nwh);
		}
		else
		{
			_swapChain->swapBuffers();
		}
	}

	void GlContext::makeCurrent(SwapChainGL* _swapChain)
	{
		if (m_current != _swapChain)
		{
			m_current = _swapChain;

			if (NULL == _swapChain)
			{
				glXMakeCurrent( (::Display*)g_platformData.ndt, (::Window)g_platformData.nwh, m_context);
			}
			else
			{
				_swapChain->makeCurrent();
			}
		}
	}

	void GlContext::import()
	{
#	define GL_EXTENSION(_optional, _proto, _func, _import) \
				{ \
					if (NULL == _func) \
					{ \
						_func = (_proto)glXGetProcAddress( (const GLubyte*)#_import); \
						BX_TRACE("%p " #_func " (" #_import ")", _func); \
						BGFX_FATAL(_optional || NULL != _func, Fatal::UnableToInitialize, "Failed to create OpenGL context. glXGetProcAddress %s", #_import); \
					} \
				}
#	include "glimports.h"
	}

} /* namespace gl */ } // namespace bgfx

#	endif // BGFX_USE_GLX

#endif // (BX_PLATFORM_FREEBSD || BX_PLATFORM_LINUX) && (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BX_PLATFORM_NACL && (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
#	include <bgfxplatform.h>
#	include "renderer_gl.h"

namespace bgfx { namespace gl
{
#	define GL_IMPORT(_optional, _proto, _func, _import) _proto _func
#	include "glimports.h"

	void naclSwapCompleteCb(void* /*_data*/, int32_t /*_result*/);

	PP_CompletionCallback naclSwapComplete =
	{
		naclSwapCompleteCb,
		NULL,
		PP_COMPLETIONCALLBACK_FLAG_NONE
	};

	struct Ppapi
	{
		Ppapi()
			: m_context(0)
			, m_instance(0)
			, m_instInterface(NULL)
			, m_graphicsInterface(NULL)
			, m_instancedArrays(NULL)
			, m_postSwapBuffers(NULL)
			, m_forceSwap(true)
		{
		}

		bool setInterfaces(PP_Instance _instance, const PPB_Instance* _instInterface, const PPB_Graphics3D* _graphicsInterface, PostSwapBuffersFn _postSwapBuffers);

		void resize(uint32_t _width, uint32_t _height, uint32_t /*_flags*/)
		{
			m_graphicsInterface->ResizeBuffers(m_context, _width, _height);
		}

		void swap()
		{
			glSetCurrentContextPPAPI(m_context);
			m_graphicsInterface->SwapBuffers(m_context, naclSwapComplete);
		}

		bool isValid() const
		{
			return 0 != m_context;
		}

		PP_Resource m_context;
		PP_Instance m_instance;
		const PPB_Instance* m_instInterface;
		const PPB_Graphics3D* m_graphicsInterface;
		const PPB_OpenGLES2InstancedArrays* m_instancedArrays;
		PostSwapBuffersFn m_postSwapBuffers;
		bool m_forceSwap;
	};

	static Ppapi s_ppapi;

	void naclSwapCompleteCb(void* /*_data*/, int32_t /*_result*/)
	{
		// For NaCl bgfx doesn't create render thread, but rendering is always
		// multithreaded. Frame rendering is done on main thread, and context
		// is initialized when PPAPI interfaces are set. Force swap is there to
		// keep calling swap complete callback, so that initialization doesn't
		// deadlock on semaphores.
		if (s_ppapi.m_forceSwap)
		{
			s_ppapi.swap();
		}

		renderFrame();
	}

	static void GL_APIENTRY naclVertexAttribDivisor(GLuint _index, GLuint _divisor)
	{
		s_ppapi.m_instancedArrays->VertexAttribDivisorANGLE(s_ppapi.m_context, _index, _divisor);
	}

	static void GL_APIENTRY naclDrawArraysInstanced(GLenum _mode, GLint _first, GLsizei _count, GLsizei _primcount)
	{
		s_ppapi.m_instancedArrays->DrawArraysInstancedANGLE(s_ppapi.m_context, _mode, _first, _count, _primcount);
	}

	static void GL_APIENTRY naclDrawElementsInstanced(GLenum _mode, GLsizei _count, GLenum _type, const GLvoid* _indices, GLsizei _primcount)
	{
		s_ppapi.m_instancedArrays->DrawElementsInstancedANGLE(s_ppapi.m_context, _mode, _count, _type, _indices, _primcount);
	}

	bool Ppapi::setInterfaces(PP_Instance _instance, const PPB_Instance* _instInterface, const PPB_Graphics3D* _graphicsInterface, PostSwapBuffersFn _postSwapBuffers)
	{
		BX_TRACE("PPAPI Interfaces");

		m_instance = _instance;
		m_instInterface = _instInterface;
		m_graphicsInterface = _graphicsInterface;
		m_instancedArrays = glGetInstancedArraysInterfacePPAPI();
		m_postSwapBuffers = _postSwapBuffers;

		int32_t attribs[] =
		{
			PP_GRAPHICS3DATTRIB_ALPHA_SIZE, 8,
			PP_GRAPHICS3DATTRIB_DEPTH_SIZE, 24,
			PP_GRAPHICS3DATTRIB_STENCIL_SIZE, 8,
			PP_GRAPHICS3DATTRIB_SAMPLES, 0,
			PP_GRAPHICS3DATTRIB_SAMPLE_BUFFERS, 0,
			PP_GRAPHICS3DATTRIB_WIDTH, BGFX_DEFAULT_WIDTH,
			PP_GRAPHICS3DATTRIB_HEIGHT, BGFX_DEFAULT_HEIGHT,
			PP_GRAPHICS3DATTRIB_NONE
		};

		m_context = m_graphicsInterface->Create(m_instance, 0, attribs);
		if (0 == m_context)
		{
			BX_TRACE("Failed to create context!");
			return false;
		}

		m_instInterface->BindGraphics(m_instance, m_context);
		glSetCurrentContextPPAPI(m_context);
		m_graphicsInterface->SwapBuffers(m_context, naclSwapComplete);

		glVertexAttribDivisor   = naclVertexAttribDivisor;
		glDrawArraysInstanced   = naclDrawArraysInstanced;
		glDrawElementsInstanced = naclDrawElementsInstanced;

		// Prevent render thread creation.
		RenderFrame::Enum result = renderFrame();
		return RenderFrame::NoContext == result;
	}

	void GlContext::create(uint32_t _width, uint32_t _height)
	{
		BX_UNUSED(_width, _height);
		BX_TRACE("GlContext::create");
	}

	void GlContext::destroy()
	{
	}

	void GlContext::resize(uint32_t _width, uint32_t _height, uint32_t _flags)
	{
		s_ppapi.m_forceSwap = false;
		s_ppapi.resize(_width, _height, _flags);
	}

	uint64_t GlContext::getCaps() const
	{
		return 0;
	}

	SwapChainGL* GlContext::createSwapChain(void* /*_nwh*/)
	{
		BX_CHECK(false, "Shouldn't be called!");
		return NULL;
	}

	void GlContext::destroySwapChain(SwapChainGL*  /*_swapChain*/)
	{
		BX_CHECK(false, "Shouldn't be called!");
	}

	void GlContext::swap(SwapChainGL* /*_swapChain*/)
	{
		s_ppapi.swap();
	}

	void GlContext::makeCurrent(SwapChainGL* /*_swapChain*/)
	{
	}

	void GlContext::import()
	{
	}

	bool GlContext::isValid() const
	{
		return s_ppapi.isValid();
	}

} /* namespace gl */ } // namespace bgfx

namespace bgfx
{
	bool naclSetInterfaces(PP_Instance _instance, const PPB_Instance* _instInterface, const PPB_Graphics3D* _graphicsInterface, PostSwapBuffersFn _postSwapBuffers)
	{
		return gl::s_ppapi.setInterfaces( _instance, _instInterface, _graphicsInterface, _postSwapBuffers);
	}
} // namespace bgfx

#endif // BX_PLATFORM_NACL && (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if (BGFX_CONFIG_RENDERER_OPENGLES|BGFX_CONFIG_RENDERER_OPENGL)
#	include "renderer_gl.h"

#	if BGFX_USE_WGL

namespace bgfx { namespace gl
{
	PFNWGLGETPROCADDRESSPROC wglGetProcAddress;
	PFNWGLMAKECURRENTPROC wglMakeCurrent;
	PFNWGLCREATECONTEXTPROC wglCreateContext;
	PFNWGLDELETECONTEXTPROC wglDeleteContext;
	PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB;
	PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB;
	PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB;
	PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

#	define GL_IMPORT(_optional, _proto, _func, _import) _proto _func
#	include "glimports.h"

	struct SwapChainGL
	{
		SwapChainGL(void* _nwh)
			: m_hwnd( (HWND)_nwh)
		{
			m_hdc = GetDC(m_hwnd);
		}

		~SwapChainGL()
		{
			wglMakeCurrent(NULL, NULL);
			wglDeleteContext(m_context);
			ReleaseDC(m_hwnd, m_hdc);
		}

		void makeCurrent()
		{
			wglMakeCurrent(m_hdc, m_context);
			GLenum err = glGetError();
			BX_WARN(0 == err, "wglMakeCurrent failed with GL error: 0x%04x.", err); BX_UNUSED(err);
		}

		void swapBuffers()
		{
			SwapBuffers(m_hdc);
		}

		HWND  m_hwnd;
		HDC   m_hdc;
		HGLRC m_context;
	};

	static HGLRC createContext(HDC _hdc)
	{
		PIXELFORMATDESCRIPTOR pfd;
		memset(&pfd, 0, sizeof(pfd) );
		pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		pfd.cAlphaBits = 8;
		pfd.cDepthBits = 24;
		pfd.cStencilBits = 8;
		pfd.iLayerType = PFD_MAIN_PLANE;

		int pixelFormat = ChoosePixelFormat(_hdc, &pfd);
		BGFX_FATAL(0 != pixelFormat, Fatal::UnableToInitialize, "ChoosePixelFormat failed!");

		DescribePixelFormat(_hdc, pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd);

		BX_TRACE("Pixel format:\n"
			"\tiPixelType %d\n"
			"\tcColorBits %d\n"
			"\tcAlphaBits %d\n"
			"\tcDepthBits %d\n"
			"\tcStencilBits %d\n"
			, pfd.iPixelType
			, pfd.cColorBits
			, pfd.cAlphaBits
			, pfd.cDepthBits
			, pfd.cStencilBits
			);

		int result;
		result = SetPixelFormat(_hdc, pixelFormat, &pfd);
		BGFX_FATAL(0 != result, Fatal::UnableToInitialize, "SetPixelFormat failed!");

		HGLRC context = wglCreateContext(_hdc);
		BGFX_FATAL(NULL != context, Fatal::UnableToInitialize, "wglCreateContext failed!");

		result = wglMakeCurrent(_hdc, context);
		BGFX_FATAL(0 != result, Fatal::UnableToInitialize, "wglMakeCurrent failed!");

		return context;
	}

	void GlContext::create(uint32_t /*_width*/, uint32_t /*_height*/)
	{
		m_opengl32dll = bx::dlopen("opengl32.dll");
		BGFX_FATAL(NULL != m_opengl32dll, Fatal::UnableToInitialize, "Failed to load opengl32.dll.");

		wglGetProcAddress = (PFNWGLGETPROCADDRESSPROC)bx::dlsym(m_opengl32dll, "wglGetProcAddress");
		BGFX_FATAL(NULL != wglGetProcAddress, Fatal::UnableToInitialize, "Failed get wglGetProcAddress.");

		// If g_platformHooks.nwh is NULL, the assumption is that GL context was created
		// by user (for example, using SDL, GLFW, etc.)
		BX_WARN(NULL != g_platformData.nwh
			, "bgfx::setPlatform with valid window is not called. This might "
			  "be intentional when GL context is created by the user."
			);

		if (NULL != g_platformData.nwh)
		{
			wglMakeCurrent = (PFNWGLMAKECURRENTPROC)bx::dlsym(m_opengl32dll, "wglMakeCurrent");
			BGFX_FATAL(NULL != wglMakeCurrent, Fatal::UnableToInitialize, "Failed get wglMakeCurrent.");

			wglCreateContext = (PFNWGLCREATECONTEXTPROC)bx::dlsym(m_opengl32dll, "wglCreateContext");
			BGFX_FATAL(NULL != wglCreateContext, Fatal::UnableToInitialize, "Failed get wglCreateContext.");

			wglDeleteContext = (PFNWGLDELETECONTEXTPROC)bx::dlsym(m_opengl32dll, "wglDeleteContext");
			BGFX_FATAL(NULL != wglDeleteContext, Fatal::UnableToInitialize, "Failed get wglDeleteContext.");

			m_hdc = GetDC( (HWND)g_platformData.nwh);
			BGFX_FATAL(NULL != m_hdc, Fatal::UnableToInitialize, "GetDC failed!");

			// Dummy window to peek into WGL functionality.
			//
			// An application can only set the pixel format of a window one time.
			// Once a window's pixel format is set, it cannot be changed.
			// MSDN: http://msdn.microsoft.com/en-us/library/windows/desktop/dd369049%28v=vs.85%29.aspx
			HWND hwnd = CreateWindowA("STATIC"
				, ""
				, WS_POPUP|WS_DISABLED
				, -32000
				, -32000
				, 0
				, 0
				, NULL
				, NULL
				, GetModuleHandle(NULL)
				, 0
				);

			HDC hdc = GetDC(hwnd);
			BGFX_FATAL(NULL != hdc, Fatal::UnableToInitialize, "GetDC failed!");

			HGLRC context = createContext(hdc);

			wglGetExtensionsStringARB  = (PFNWGLGETEXTENSIONSSTRINGARBPROC)wglGetProcAddress("wglGetExtensionsStringARB");
			wglChoosePixelFormatARB    = (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
			wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
			wglSwapIntervalEXT         = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");

			if (NULL != wglGetExtensionsStringARB)
			{
				const char* extensions = (const char*)wglGetExtensionsStringARB(hdc);
				BX_TRACE("WGL extensions:");
				dumpExtensions(extensions);
			}

			if (NULL != wglChoosePixelFormatARB
			&&  NULL != wglCreateContextAttribsARB)
			{
				int32_t attrs[] =
				{
					WGL_SAMPLE_BUFFERS_ARB, 0,
					WGL_SAMPLES_ARB, 0,
					WGL_SUPPORT_OPENGL_ARB, true,
					WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
					WGL_DRAW_TO_WINDOW_ARB, true,
					WGL_DOUBLE_BUFFER_ARB, true,
					WGL_COLOR_BITS_ARB, 32,
					WGL_DEPTH_BITS_ARB, 24,
					WGL_STENCIL_BITS_ARB, 8,
					0
				};

				int result;
				uint32_t numFormats = 0;
				do
				{
					result = wglChoosePixelFormatARB(m_hdc, attrs, NULL, 1, &m_pixelFormat, &numFormats);
					if (0 == result
					||  0 == numFormats)
					{
						attrs[3] >>= 1;
						attrs[1] = attrs[3] == 0 ? 0 : 1;
					}

				} while (0 == numFormats);

				DescribePixelFormat(m_hdc, m_pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &m_pfd);

				BX_TRACE("Pixel format:\n"
					"\tiPixelType %d\n"
					"\tcColorBits %d\n"
					"\tcAlphaBits %d\n"
					"\tcDepthBits %d\n"
					"\tcStencilBits %d\n"
					, m_pfd.iPixelType
					, m_pfd.cColorBits
					, m_pfd.cAlphaBits
					, m_pfd.cDepthBits
					, m_pfd.cStencilBits
					);

				result = SetPixelFormat(m_hdc, m_pixelFormat, &m_pfd);
				// When window is created by SDL and SDL_WINDOW_OPENGL is set, SetPixelFormat
				// will fail. Just warn and continue. In case it failed for some other reason
				// create context will fail and it will error out there.
				BX_WARN(result, "SetPixelFormat failed (last err: 0x%08x)!", GetLastError() );

				int32_t flags = BGFX_CONFIG_DEBUG ? WGL_CONTEXT_DEBUG_BIT_ARB : 0;
				BX_UNUSED(flags);
				int32_t contextAttrs[9] =
				{
#if BGFX_CONFIG_RENDERER_OPENGL >= 31
					WGL_CONTEXT_MAJOR_VERSION_ARB, BGFX_CONFIG_RENDERER_OPENGL / 10,
					WGL_CONTEXT_MINOR_VERSION_ARB, BGFX_CONFIG_RENDERER_OPENGL % 10,
					WGL_CONTEXT_FLAGS_ARB, flags,
					WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#else
					WGL_CONTEXT_MAJOR_VERSION_ARB, 2,
					WGL_CONTEXT_MINOR_VERSION_ARB, 1,
					0, 0,
					0, 0,
#endif // BGFX_CONFIG_RENDERER_OPENGL >= 31
					0
				};

				m_context = wglCreateContextAttribsARB(m_hdc, 0, contextAttrs);
				if (NULL == m_context)
				{
					// nVidia doesn't like context profile mask for contexts below 3.2?
					contextAttrs[6] = WGL_CONTEXT_PROFILE_MASK_ARB == contextAttrs[6] ? 0 : contextAttrs[6];
					m_context = wglCreateContextAttribsARB(m_hdc, 0, contextAttrs);
				}
				BGFX_FATAL(NULL != m_context, Fatal::UnableToInitialize, "Failed to create context 0x%08x.", GetLastError() );

				BX_STATIC_ASSERT(sizeof(contextAttrs) == sizeof(m_contextAttrs) );
				memcpy(m_contextAttrs, contextAttrs, sizeof(contextAttrs) );
			}

			wglMakeCurrent(NULL, NULL);
			wglDeleteContext(context);
			DestroyWindow(hwnd);

			if (NULL == m_context)
			{
				m_context = createContext(m_hdc);
			}

			int result = wglMakeCurrent(m_hdc, m_context);
			BGFX_FATAL(0 != result, Fatal::UnableToInitialize, "wglMakeCurrent failed!");
			m_current = NULL;

			if (NULL != wglSwapIntervalEXT)
			{
				wglSwapIntervalEXT(0);
			}
		}

		import();
	}

	void GlContext::destroy()
	{
		if (NULL != g_platformData.nwh)
		{
			wglMakeCurrent(NULL, NULL);

			wglDeleteContext(m_context);
			m_context = NULL;

			ReleaseDC( (HWND)g_platformData.nwh, m_hdc);
			m_hdc = NULL;
		}

		bx::dlclose(m_opengl32dll);
		m_opengl32dll = NULL;
	}

	void GlContext::resize(uint32_t /*_width*/, uint32_t /*_height*/, uint32_t _flags)
	{
		if (NULL != wglSwapIntervalEXT)
		{
			bool vsync = !!(_flags&BGFX_RESET_VSYNC);
			wglSwapIntervalEXT(vsync ? 1 : 0);
		}
	}

	uint64_t GlContext::getCaps() const
	{
		return BGFX_CAPS_SWAP_CHAIN;
	}

	SwapChainGL* GlContext::createSwapChain(void* _nwh)
	{
		SwapChainGL* swapChain = BX_NEW(g_allocator, SwapChainGL)(_nwh);

		int result = SetPixelFormat(swapChain->m_hdc, m_pixelFormat, &m_pfd);
		BX_WARN(result, "SetPixelFormat failed (last err: 0x%08x)!", GetLastError() ); BX_UNUSED(result);

		swapChain->m_context = wglCreateContextAttribsARB(swapChain->m_hdc, m_context, m_contextAttrs);
		BX_CHECK(NULL != swapChain->m_context, "Create swap chain failed: %x", glGetError() );
		return swapChain;
	}

	void GlContext::destroySwapChain(SwapChainGL*  _swapChain)
	{
		BX_DELETE(g_allocator, _swapChain);
	}

	void GlContext::swap(SwapChainGL* _swapChain)
	{
		makeCurrent(_swapChain);

		if (NULL == _swapChain)
		{
			if (NULL != g_platformData.nwh)
			{
				SwapBuffers(m_hdc);
			}
		}
		else
		{
			_swapChain->swapBuffers();
		}
	}

	void GlContext::makeCurrent(SwapChainGL* _swapChain)
	{
		if (m_current != _swapChain)
		{
			m_current = _swapChain;

			if (NULL == _swapChain)
			{
				wglMakeCurrent(m_hdc, m_context);
				GLenum err = glGetError();
				BX_WARN(0 == err, "wglMakeCurrent failed with GL error: 0x%04x.", err); BX_UNUSED(err);
			}
			else
			{
				_swapChain->makeCurrent();
			}
		}
	}

	void GlContext::import()
	{
		BX_TRACE("Import:");
#	define GL_EXTENSION(_optional, _proto, _func, _import) \
				{ \
					if (NULL == _func) \
					{ \
						_func = (_proto)wglGetProcAddress(#_import); \
						if (_func == NULL) \
						{ \
							_func = (_proto)bx::dlsym(m_opengl32dll, #_import); \
							BX_TRACE("    %p " #_func " (" #_import ")", _func); \
						} \
						else \
						{ \
							BX_TRACE("wgl %p " #_func " (" #_import ")", _func); \
						} \
						BGFX_FATAL(BX_IGNORE_C4127(_optional) || NULL != _func, Fatal::UnableToInitialize, "Failed to create OpenGL context. wglGetProcAddress(\"%s\")", #_import); \
					} \
				}
#	include "glimports.h"
	}

} } // namespace bgfx

#	endif // BGFX_USE_WGL
#endif // (BGFX_CONFIG_RENDERER_OPENGLES|BGFX_CONFIG_RENDERER_OPENGL)
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"
#include <math.h> // powf, sqrtf

#include "image.h"

namespace bgfx
{
	static const ImageBlockInfo s_imageBlockInfo[] =
	{
		//  +------------------------------- bits per pixel
		//  |  +---------------------------- block width
		//  |  |  +------------------------- block height
		//  |  |  |   +--------------------- block size
		//  |  |  |   |  +------------------ min blocks x
		//  |  |  |   |  |  +--------------- min blocks y
		//  |  |  |   |  |  |   +----------- depth bits
		//  |  |  |   |  |  |   |  +-------- stencil bits
		//  |  |  |   |  |  |   |  |  +----- encoding type
		//  |  |  |   |  |  |   |  |  |
		{   4, 4, 4,  8, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC1
		{   8, 4, 4, 16, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC2
		{   8, 4, 4, 16, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC3
		{   4, 4, 4,  8, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC4
		{   8, 4, 4, 16, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC5
		{   8, 4, 4, 16, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC6H
		{   8, 4, 4, 16, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BC7
		{   4, 4, 4,  8, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // ETC1
		{   4, 4, 4,  8, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // ETC2
		{   8, 4, 4, 16, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // ETC2A
		{   4, 4, 4,  8, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // ETC2A1
		{   2, 8, 4,  8, 2, 2,  0, 0, uint8_t(EncodingType::Unorm) }, // PTC12
		{   4, 4, 4,  8, 2, 2,  0, 0, uint8_t(EncodingType::Unorm) }, // PTC14
		{   2, 8, 4,  8, 2, 2,  0, 0, uint8_t(EncodingType::Unorm) }, // PTC12A
		{   4, 4, 4,  8, 2, 2,  0, 0, uint8_t(EncodingType::Unorm) }, // PTC14A
		{   2, 8, 4,  8, 2, 2,  0, 0, uint8_t(EncodingType::Unorm) }, // PTC22
		{   4, 4, 4,  8, 2, 2,  0, 0, uint8_t(EncodingType::Unorm) }, // PTC24
		{   0, 0, 0,  0, 0, 0,  0, 0, uint8_t(EncodingType::Count) }, // Unknown
		{   1, 8, 1,  1, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // R1
		{   8, 1, 1,  1, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // R8
		{   8, 1, 1,  1, 1, 1,  0, 0, uint8_t(EncodingType::Int  ) }, // R8I
		{   8, 1, 1,  1, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // R8U
		{   8, 1, 1,  1, 1, 1,  0, 0, uint8_t(EncodingType::Snorm) }, // R8S
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // R16
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Int  ) }, // R16I
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // R16U
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Float) }, // R16F
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Snorm) }, // R16S
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // R32U
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Float) }, // R32F
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RG8
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Int  ) }, // RG8I
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // RG8U
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Snorm) }, // RG8S
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RG16
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Int  ) }, // RG16I
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // RG16U
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Float) }, // RG16F
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Snorm) }, // RG16S
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // RG32U
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Float) }, // RG32F
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // BGRA8
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RGBA8
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Int  ) }, // RGBA8I
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // RGBA8U
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Snorm) }, // RGBA8S
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RGBA16
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Int  ) }, // RGBA16I
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // RGBA16U
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Float) }, // RGBA16F
		{  64, 1, 1,  8, 1, 1,  0, 0, uint8_t(EncodingType::Snorm) }, // RGBA16S
		{ 128, 1, 1, 16, 1, 1,  0, 0, uint8_t(EncodingType::Uint ) }, // RGBA32U
		{ 128, 1, 1, 16, 1, 1,  0, 0, uint8_t(EncodingType::Float) }, // RGBA32F
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // R5G6B5
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RGBA4
		{  16, 1, 1,  2, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RGB5A1
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // RGB10A2
		{  32, 1, 1,  4, 1, 1,  0, 0, uint8_t(EncodingType::Unorm) }, // R11G11B10F
		{   0, 0, 0,  0, 0, 0,  0, 0, uint8_t(EncodingType::Count) }, // UnknownDepth
		{  16, 1, 1,  2, 1, 1, 16, 0, uint8_t(EncodingType::Unorm) }, // D16
		{  24, 1, 1,  3, 1, 1, 24, 0, uint8_t(EncodingType::Unorm) }, // D24
		{  32, 1, 1,  4, 1, 1, 24, 8, uint8_t(EncodingType::Unorm) }, // D24S8
		{  32, 1, 1,  4, 1, 1, 32, 0, uint8_t(EncodingType::Unorm) }, // D32
		{  16, 1, 1,  2, 1, 1, 16, 0, uint8_t(EncodingType::Unorm) }, // D16F
		{  24, 1, 1,  3, 1, 1, 24, 0, uint8_t(EncodingType::Unorm) }, // D24F
		{  32, 1, 1,  4, 1, 1, 32, 0, uint8_t(EncodingType::Unorm) }, // D32F
		{   8, 1, 1,  1, 1, 1,  0, 8, uint8_t(EncodingType::Unorm) }, // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_imageBlockInfo) );

	static const char* s_textureFormatName[] =
	{
		"BC1",        // BC1
		"BC2",        // BC2
		"BC3",        // BC3
		"BC4",        // BC4
		"BC5",        // BC5
		"BC6H",       // BC6H
		"BC7",        // BC7
		"ETC1",       // ETC1
		"ETC2",       // ETC2
		"ETC2A",      // ETC2A
		"ETC2A1",     // ETC2A1
		"PTC12",      // PTC12
		"PTC14",      // PTC14
		"PTC12A",     // PTC12A
		"PTC14A",     // PTC14A
		"PTC22",      // PTC22
		"PTC24",      // PTC24
		"<unknown>",  // Unknown
		"R1",         // R1
		"R8",         // R8
		"R8I",        // R8I
		"R8U",        // R8U
		"R8S",        // R8S
		"R16",        // R16
		"R16I",       // R16I
		"R16U",       // R16U
		"R16F",       // R16F
		"R16S",       // R16S
		"R32U",       // R32U
		"R32F",       // R32F
		"RG8",        // RG8
		"RG8I",       // RG8I
		"RG8U",       // RG8U
		"RG8S",       // RG8S
		"RG16",       // RG16
		"RG16I",      // RG16I
		"RG16U",      // RG16U
		"RG16F",      // RG16F
		"RG16S",      // RG16S
		"RG32",       // RG32U
		"RG32F",      // RG32F
		"BGRA8",      // BGRA8
		"RGBA8",      // RGBA8
		"RGBA8I",     // RGBA8I
		"RGBA8U",     // RGBA8U
		"RGBA8S",     // RGBA8S
		"RGBA16",     // RGBA16
		"RGBA16I",    // RGBA16I
		"RGBA16U",    // RGBA16U
		"RGBA16F",    // RGBA16F
		"RGBA16S",    // RGBA16S
		"RGBA32",     // RGBA32U
		"RGBA32F",    // RGBA32F
		"R5G6B5",     // R5G6B5
		"RGBA4",      // RGBA4
		"RGB5A1",     // RGB5A1
		"RGB10A2",    // RGB10A2
		"R11G11B10F", // R11G11B10F
		"<unknown>",  // UnknownDepth
		"D16",        // D16
		"D24",        // D24
		"D24S8",      // D24S8
		"D32",        // D32
		"D16F",       // D16F
		"D24F",       // D24F
		"D32F",       // D32F
		"D0S8",       // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_textureFormatName) );

	bool isCompressed(TextureFormat::Enum _format)
	{
		return _format < TextureFormat::Unknown;
	}

	bool isColor(TextureFormat::Enum _format)
	{
		return _format > TextureFormat::Unknown
			&& _format < TextureFormat::UnknownDepth
			;
	}

	bool isDepth(TextureFormat::Enum _format)
	{
		return _format > TextureFormat::UnknownDepth
			&& _format < TextureFormat::Count
			;
	}

	uint8_t getBitsPerPixel(TextureFormat::Enum _format)
	{
		return s_imageBlockInfo[_format].bitsPerPixel;
	}

	const ImageBlockInfo& getBlockInfo(TextureFormat::Enum _format)
	{
		return s_imageBlockInfo[_format];
	}

	uint8_t getBlockSize(TextureFormat::Enum _format)
	{
		return s_imageBlockInfo[_format].blockSize;
	}

	const char* getName(TextureFormat::Enum _format)
	{
		return s_textureFormatName[_format];
	}

	void imageSolid(uint32_t _width, uint32_t _height, uint32_t _solid, void* _dst)
	{
		uint32_t* dst = (uint32_t*)_dst;
		for (uint32_t ii = 0, num = _width*_height; ii < num; ++ii)
		{
			*dst++ = _solid;
		}
	}

	void imageCheckerboard(uint32_t _width, uint32_t _height, uint32_t _step, uint32_t _0, uint32_t _1, void* _dst)
	{
		uint32_t* dst = (uint32_t*)_dst;
		for (uint32_t yy = 0; yy < _height; ++yy)
		{
			for (uint32_t xx = 0; xx < _width; ++xx)
			{
				uint32_t abgr = ( (xx/_step)&1) ^ ( (yy/_step)&1) ? _1 : _0;
				*dst++ = abgr;
			}
		}
	}

	void imageRgba8Downsample2x2Ref(uint32_t _width, uint32_t _height, uint32_t _srcPitch, const void* _src, void* _dst)
	{
		const uint32_t dstwidth  = _width/2;
		const uint32_t dstheight = _height/2;

		if (0 == dstwidth
		||  0 == dstheight)
		{
			return;
		}

		uint8_t* dst = (uint8_t*)_dst;
		const uint8_t* src = (const uint8_t*)_src;

		for (uint32_t yy = 0, ystep = _srcPitch*2; yy < dstheight; ++yy, src += ystep)
		{
			const uint8_t* rgba = src;
			for (uint32_t xx = 0; xx < dstwidth; ++xx, rgba += 8, dst += 4)
			{
				float rr = powf(rgba[          0], 2.2f);
				float gg = powf(rgba[          1], 2.2f);
				float bb = powf(rgba[          2], 2.2f);
				float aa =      rgba[          3];
				rr      += powf(rgba[          4], 2.2f);
				gg      += powf(rgba[          5], 2.2f);
				bb      += powf(rgba[          6], 2.2f);
				aa      +=      rgba[          7];
				rr      += powf(rgba[_srcPitch+0], 2.2f);
				gg      += powf(rgba[_srcPitch+1], 2.2f);
				bb      += powf(rgba[_srcPitch+2], 2.2f);
				aa      +=      rgba[_srcPitch+3];
				rr      += powf(rgba[_srcPitch+4], 2.2f);
				gg      += powf(rgba[_srcPitch+5], 2.2f);
				bb      += powf(rgba[_srcPitch+6], 2.2f);
				aa      +=      rgba[_srcPitch+7];

				rr *= 0.25f;
				gg *= 0.25f;
				bb *= 0.25f;
				aa *= 0.25f;
				rr = powf(rr, 1.0f/2.2f);
				gg = powf(gg, 1.0f/2.2f);
				bb = powf(bb, 1.0f/2.2f);
				dst[0] = (uint8_t)rr;
				dst[1] = (uint8_t)gg;
				dst[2] = (uint8_t)bb;
				dst[3] = (uint8_t)aa;
			}
		}
	}

	void imageRgba8Downsample2x2(uint32_t _width, uint32_t _height, uint32_t _srcPitch, const void* _src, void* _dst)
	{
		const uint32_t dstwidth  = _width/2;
		const uint32_t dstheight = _height/2;

		if (0 == dstwidth
		||  0 == dstheight)
		{
			return;
		}

		uint8_t* dst = (uint8_t*)_dst;
		const uint8_t* src = (const uint8_t*)_src;

		using namespace bx;
		const float4_t unpack = float4_ld(1.0f, 1.0f/256.0f, 1.0f/65536.0f, 1.0f/16777216.0f);
		const float4_t pack   = float4_ld(1.0f, 256.0f*0.5f, 65536.0f, 16777216.0f*0.5f);
		const float4_t umask  = float4_ild(0xff, 0xff00, 0xff0000, 0xff000000);
		const float4_t pmask  = float4_ild(0xff, 0x7f80, 0xff0000, 0x7f800000);
		const float4_t wflip  = float4_ild(0, 0, 0, 0x80000000);
		const float4_t wadd   = float4_ld(0.0f, 0.0f, 0.0f, 32768.0f*65536.0f);
		const float4_t gamma  = float4_ld(1.0f/2.2f, 1.0f/2.2f, 1.0f/2.2f, 1.0f);
		const float4_t linear = float4_ld(2.2f, 2.2f, 2.2f, 1.0f);
		const float4_t quater = float4_splat(0.25f);

		for (uint32_t yy = 0, ystep = _srcPitch*2; yy < dstheight; ++yy, src += ystep)
		{
			const uint8_t* rgba = src;
			for (uint32_t xx = 0; xx < dstwidth; ++xx, rgba += 8, dst += 4)
			{
				const float4_t abgr0  = float4_splat(rgba);
				const float4_t abgr1  = float4_splat(rgba+4);
				const float4_t abgr2  = float4_splat(rgba+_srcPitch);
				const float4_t abgr3  = float4_splat(rgba+_srcPitch+4);

				const float4_t abgr0m = float4_and(abgr0, umask);
				const float4_t abgr1m = float4_and(abgr1, umask);
				const float4_t abgr2m = float4_and(abgr2, umask);
				const float4_t abgr3m = float4_and(abgr3, umask);
				const float4_t abgr0x = float4_xor(abgr0m, wflip);
				const float4_t abgr1x = float4_xor(abgr1m, wflip);
				const float4_t abgr2x = float4_xor(abgr2m, wflip);
				const float4_t abgr3x = float4_xor(abgr3m, wflip);
				const float4_t abgr0f = float4_itof(abgr0x);
				const float4_t abgr1f = float4_itof(abgr1x);
				const float4_t abgr2f = float4_itof(abgr2x);
				const float4_t abgr3f = float4_itof(abgr3x);
				const float4_t abgr0c = float4_add(abgr0f, wadd);
				const float4_t abgr1c = float4_add(abgr1f, wadd);
				const float4_t abgr2c = float4_add(abgr2f, wadd);
				const float4_t abgr3c = float4_add(abgr3f, wadd);
				const float4_t abgr0n = float4_mul(abgr0c, unpack);
				const float4_t abgr1n = float4_mul(abgr1c, unpack);
				const float4_t abgr2n = float4_mul(abgr2c, unpack);
				const float4_t abgr3n = float4_mul(abgr3c, unpack);

				const float4_t abgr0l = float4_pow(abgr0n, linear);
				const float4_t abgr1l = float4_pow(abgr1n, linear);
				const float4_t abgr2l = float4_pow(abgr2n, linear);
				const float4_t abgr3l = float4_pow(abgr3n, linear);

				const float4_t sum0   = float4_add(abgr0l, abgr1l);
				const float4_t sum1   = float4_add(abgr2l, abgr3l);
				const float4_t sum2   = float4_add(sum0, sum1);
				const float4_t avg0   = float4_mul(sum2, quater);
				const float4_t avg1   = float4_pow(avg0, gamma);

				const float4_t avg2   = float4_mul(avg1, pack);
				const float4_t ftoi0  = float4_ftoi(avg2);
				const float4_t ftoi1  = float4_and(ftoi0, pmask);
				const float4_t zwxy   = float4_swiz_zwxy(ftoi1);
				const float4_t tmp0   = float4_or(ftoi1, zwxy);
				const float4_t yyyy   = float4_swiz_yyyy(tmp0);
				const float4_t tmp1   = float4_iadd(yyyy, yyyy);
				const float4_t result = float4_or(tmp0, tmp1);

				float4_stx(dst, result);
			}
		}
	}

	void imageSwizzleBgra8Ref(uint32_t _width, uint32_t _height, uint32_t _srcPitch, const void* _src, void* _dst)
	{
		const uint8_t* src = (uint8_t*) _src;
		const uint8_t* next = src + _srcPitch;
		uint8_t* dst = (uint8_t*)_dst;

		for (uint32_t yy = 0; yy < _height; ++yy, src = next, next += _srcPitch)
		{
			for (uint32_t xx = 0; xx < _width; ++xx, src += 4, dst += 4)
			{
				uint8_t rr = src[0];
				uint8_t gg = src[1];
				uint8_t bb = src[2];
				uint8_t aa = src[3];
				dst[0] = bb;
				dst[1] = gg;
				dst[2] = rr;
				dst[3] = aa;
			}
		}
	}

	void imageSwizzleBgra8(uint32_t _width, uint32_t _height, uint32_t _srcPitch, const void* _src, void* _dst)
	{
		// Test can we do four 4-byte pixels at the time.
		if (0 != (_width&0x3)
		||  _width < 4
		||  !bx::isPtrAligned(_src, 16)
		||  !bx::isPtrAligned(_dst, 16) )
		{
			BX_WARN(false, "Image swizzle is taking slow path.");
			BX_WARN(bx::isPtrAligned(_src, 16), "Source %p is not 16-byte aligned.", _src);
			BX_WARN(bx::isPtrAligned(_dst, 16), "Destination %p is not 16-byte aligned.", _dst);
			BX_WARN(_width < 4, "Image width must be multiple of 4 (width %d).", _width);
			imageSwizzleBgra8Ref(_width, _height, _srcPitch, _src, _dst);
			return;
		}

		using namespace bx;

		const float4_t mf0f0 = float4_isplat(0xff00ff00);
		const float4_t m0f0f = float4_isplat(0x00ff00ff);
		const uint8_t* src = (uint8_t*) _src;
		const uint8_t* next = src + _srcPitch;
		uint8_t* dst = (uint8_t*)_dst;

		const uint32_t width = _width/4;

		for (uint32_t yy = 0; yy < _height; ++yy, src = next, next += _srcPitch)
		{
			for (uint32_t xx = 0; xx < width; ++xx, src += 16, dst += 16)
			{
				const float4_t tabgr = float4_ld(src);
				const float4_t t00ab = float4_srl(tabgr, 16);
				const float4_t tgr00 = float4_sll(tabgr, 16);
				const float4_t tgrab = float4_or(t00ab, tgr00);
				const float4_t ta0g0 = float4_and(tabgr, mf0f0);
				const float4_t t0r0b = float4_and(tgrab, m0f0f);
				const float4_t targb = float4_or(ta0g0, t0r0b);
				float4_st(dst, targb);
			}
		}
	}

	void imageCopy(uint32_t _height, uint32_t _srcPitch, const void* _src, uint32_t _dstPitch, void* _dst)
	{
		const uint32_t pitch = bx::uint32_min(_srcPitch, _dstPitch);
		const uint8_t* src = (uint8_t*)_src;
		uint8_t* dst = (uint8_t*)_dst;

		for (uint32_t yy = 0; yy < _height; ++yy, src += _srcPitch, dst += _dstPitch)
		{
			memcpy(dst, src, pitch);
		}
	}

	void imageCopy(uint32_t _width, uint32_t _height, uint32_t _bpp, uint32_t _srcPitch, const void* _src, void* _dst)
	{
		const uint32_t dstPitch = _width*_bpp/8;
		imageCopy(_height, _srcPitch, _src, dstPitch, _dst);
	}

	void imageWriteTga(bx::WriterI* _writer, uint32_t _width, uint32_t _height, uint32_t _srcPitch, const void* _src, bool _grayscale, bool _yflip)
	{
		uint8_t type = _grayscale ? 3 : 2;
		uint8_t bpp = _grayscale ? 8 : 32;

		uint8_t header[18] = {};
		header[2] = type;
		header[12] = _width&0xff;
		header[13] = (_width>>8)&0xff;
		header[14] = _height&0xff;
		header[15] = (_height>>8)&0xff;
		header[16] = bpp;
		header[17] = 32;

		bx::write(_writer, header, sizeof(header) );

		uint32_t dstPitch = _width*bpp/8;
		if (_yflip)
		{
			uint8_t* data = (uint8_t*)_src + _srcPitch*_height - _srcPitch;
			for (uint32_t yy = 0; yy < _height; ++yy)
			{
				bx::write(_writer, data, dstPitch);
				data -= _srcPitch;
			}
		}
		else if (_srcPitch == dstPitch)
		{
			bx::write(_writer, _src, _height*_srcPitch);
		}
		else
		{
			uint8_t* data = (uint8_t*)_src;
			for (uint32_t yy = 0; yy < _height; ++yy)
			{
				bx::write(_writer, data, dstPitch);
				data += _srcPitch;
			}
		}
	}

	uint8_t bitRangeConvert(uint32_t _in, uint32_t _from, uint32_t _to)
	{
		using namespace bx;
		uint32_t tmp0   = uint32_sll(1, _to);
		uint32_t tmp1   = uint32_sll(1, _from);
		uint32_t tmp2   = uint32_dec(tmp0);
		uint32_t tmp3   = uint32_dec(tmp1);
		uint32_t tmp4   = uint32_mul(_in, tmp2);
		uint32_t tmp5   = uint32_add(tmp3, tmp4);
		uint32_t tmp6   = uint32_srl(tmp5, _from);
		uint32_t tmp7   = uint32_add(tmp5, tmp6);
		uint32_t result = uint32_srl(tmp7, _from);

		return uint8_t(result);
	}

	void decodeBlockDxt(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		uint8_t colors[4*3];

		uint32_t c0 = _src[0] | (_src[1] << 8);
		colors[0] = bitRangeConvert( (c0>> 0)&0x1f, 5, 8);
		colors[1] = bitRangeConvert( (c0>> 5)&0x3f, 6, 8);
		colors[2] = bitRangeConvert( (c0>>11)&0x1f, 5, 8);

		uint32_t c1 = _src[2] | (_src[3] << 8);
		colors[3] = bitRangeConvert( (c1>> 0)&0x1f, 5, 8);
		colors[4] = bitRangeConvert( (c1>> 5)&0x3f, 6, 8);
		colors[5] = bitRangeConvert( (c1>>11)&0x1f, 5, 8);

		colors[6] = (2*colors[0] + colors[3]) / 3;
		colors[7] = (2*colors[1] + colors[4]) / 3;
		colors[8] = (2*colors[2] + colors[5]) / 3;

		colors[ 9] = (colors[0] + 2*colors[3]) / 3;
		colors[10] = (colors[1] + 2*colors[4]) / 3;
		colors[11] = (colors[2] + 2*colors[5]) / 3;

		for (uint32_t ii = 0, next = 8*4; ii < 16*4; ii += 4, next += 2)
		{
			int idx = ( (_src[next>>3] >> (next & 7) ) & 3) * 3;
			_dst[ii+0] = colors[idx+0];
			_dst[ii+1] = colors[idx+1];
			_dst[ii+2] = colors[idx+2];
		}
	}

	void decodeBlockDxt1(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		uint8_t colors[4*4];

		uint32_t c0 = _src[0] | (_src[1] << 8);
		colors[0] = bitRangeConvert( (c0>> 0)&0x1f, 5, 8);
		colors[1] = bitRangeConvert( (c0>> 5)&0x3f, 6, 8);
		colors[2] = bitRangeConvert( (c0>>11)&0x1f, 5, 8);
		colors[3] = 255;

		uint32_t c1 = _src[2] | (_src[3] << 8);
		colors[4] = bitRangeConvert( (c1>> 0)&0x1f, 5, 8);
		colors[5] = bitRangeConvert( (c1>> 5)&0x3f, 6, 8);
		colors[6] = bitRangeConvert( (c1>>11)&0x1f, 5, 8);
		colors[7] = 255;

		if (c0 > c1)
		{
			colors[ 8] = (2*colors[0] + colors[4]) / 3;
			colors[ 9] = (2*colors[1] + colors[5]) / 3;
			colors[10] = (2*colors[2] + colors[6]) / 3;
			colors[11] = 255;

			colors[12] = (colors[0] + 2*colors[4]) / 3;
			colors[13] = (colors[1] + 2*colors[5]) / 3;
			colors[14] = (colors[2] + 2*colors[6]) / 3;
			colors[15] = 255;
		}
		else
		{
			colors[ 8] = (colors[0] + colors[4]) / 2;
			colors[ 9] = (colors[1] + colors[5]) / 2;
			colors[10] = (colors[2] + colors[6]) / 2;
			colors[11] = 255;

			colors[12] = 0;
			colors[13] = 0;
			colors[14] = 0;
			colors[15] = 0;
		}

		for (uint32_t ii = 0, next = 8*4; ii < 16*4; ii += 4, next += 2)
		{
			int idx = ( (_src[next>>3] >> (next & 7) ) & 3) * 4;
			_dst[ii+0] = colors[idx+0];
			_dst[ii+1] = colors[idx+1];
			_dst[ii+2] = colors[idx+2];
			_dst[ii+3] = colors[idx+3];
		}
	}

	void decodeBlockDxt23A(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		for (uint32_t ii = 0, next = 0; ii < 16*4; ii += 4, next += 4)
		{
			uint32_t c0 = (_src[next>>3] >> (next&7) ) & 0xf;
			_dst[ii] = bitRangeConvert(c0, 4, 8);
		}
	}

	void decodeBlockDxt45A(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		uint8_t alpha[8];
		alpha[0] = _src[0];
		alpha[1] = _src[1];

		if (alpha[0] > alpha[1])
		{
			alpha[2] = (6*alpha[0] + 1*alpha[1]) / 7;
			alpha[3] = (5*alpha[0] + 2*alpha[1]) / 7;
			alpha[4] = (4*alpha[0] + 3*alpha[1]) / 7;
			alpha[5] = (3*alpha[0] + 4*alpha[1]) / 7;
			alpha[6] = (2*alpha[0] + 5*alpha[1]) / 7;
			alpha[7] = (1*alpha[0] + 6*alpha[1]) / 7;
		}
		else
		{
			alpha[2] = (4*alpha[0] + 1*alpha[1]) / 5;
			alpha[3] = (3*alpha[0] + 2*alpha[1]) / 5;
			alpha[4] = (2*alpha[0] + 3*alpha[1]) / 5;
			alpha[5] = (1*alpha[0] + 4*alpha[1]) / 5;
			alpha[6] = 0;
			alpha[7] = 255;
		}

		uint32_t idx0 = _src[2];
		uint32_t idx1 = _src[5];
		idx0 |= uint32_t(_src[3])<<8;
		idx1 |= uint32_t(_src[6])<<8;
		idx0 |= uint32_t(_src[4])<<16;
		idx1 |= uint32_t(_src[7])<<16;
		for (uint32_t ii = 0; ii < 8*4; ii += 4)
		{
			_dst[ii]    = alpha[idx0&7];
			_dst[ii+32] = alpha[idx1&7];
			idx0 >>= 3;
			idx1 >>= 3;
		}
	}

	static const int32_t s_etc1Mod[8][4] =
	{
		{  2,   8,  -2,   -8},
		{  5,  17,  -5,  -17},
		{  9,  29,  -9,  -29},
		{ 13,  42, -13,  -42},
		{ 18,  60, -18,  -60},
		{ 24,  80, -24,  -80},
		{ 33, 106, -33, -106},
		{ 47, 183, -47, -183},
	};

	static const uint8_t s_etc2Mod[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };

	uint8_t uint8_sat(int32_t _a)
	{
		using namespace bx;
		const uint32_t min    = uint32_imin(_a, 255);
		const uint32_t result = uint32_imax(min, 0);
		return (uint8_t)result;
	}

	uint8_t uint8_satadd(int32_t _a, int32_t _b)
	{
		const int32_t add = _a + _b;
		return uint8_sat(add);
	}

	void decodeBlockEtc2ModeT(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		uint8_t rgb[16];

		// 0       1       2       3       4       5       6       7
		// 7654321076543210765432107654321076543210765432107654321076543210
		// ...rr.rrggggbbbbrrrrggggbbbbDDD.mmmmmmmmmmmmmmmmllllllllllllllll
		//    ^            ^           ^   ^               ^
		//    +-- c0       +-- c1      |   +-- msb         +-- lsb
		//                             +-- dist

		rgb[ 0] = ( (_src[0] >> 1) & 0xc)
			    |   (_src[0]       & 0x3)
			    ;
		rgb[ 1] = _src[1] >> 4;
		rgb[ 2] = _src[1] & 0xf;

		rgb[ 8] = _src[2] >> 4;
		rgb[ 9] = _src[2] & 0xf;
		rgb[10] = _src[3] >> 4;

		rgb[ 0] = bitRangeConvert(rgb[ 0], 4, 8);
		rgb[ 1] = bitRangeConvert(rgb[ 1], 4, 8);
		rgb[ 2] = bitRangeConvert(rgb[ 2], 4, 8);
		rgb[ 8] = bitRangeConvert(rgb[ 8], 4, 8);
		rgb[ 9] = bitRangeConvert(rgb[ 9], 4, 8);
		rgb[10] = bitRangeConvert(rgb[10], 4, 8);

		uint8_t dist = (_src[3] >> 1) & 0x7;
		int32_t mod = s_etc2Mod[dist];

		rgb[ 4] = uint8_satadd(rgb[ 8],  mod);
		rgb[ 5] = uint8_satadd(rgb[ 9],  mod);
		rgb[ 6] = uint8_satadd(rgb[10],  mod);

		rgb[12] = uint8_satadd(rgb[ 8], -mod);
		rgb[13] = uint8_satadd(rgb[ 9], -mod);
		rgb[14] = uint8_satadd(rgb[10], -mod);

		uint32_t indexMsb = (_src[4]<<8) | _src[5];
		uint32_t indexLsb = (_src[6]<<8) | _src[7];

		for (uint32_t ii = 0; ii < 16; ++ii)
		{
			const uint32_t idx  = (ii&0xc) | ( (ii & 0x3)<<4);
			const uint32_t lsbi = indexLsb & 1;
			const uint32_t msbi = (indexMsb & 1)<<1;
			const uint32_t pal  = (lsbi | msbi)<<2;

			_dst[idx + 0] = rgb[pal+2];
			_dst[idx + 1] = rgb[pal+1];
			_dst[idx + 2] = rgb[pal+0];
			_dst[idx + 3] = 255;

			indexLsb >>= 1;
			indexMsb >>= 1;
		}
	}

	void decodeBlockEtc2ModeH(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		uint8_t rgb[16];

		// 0       1       2       3       4       5       6       7
		// 7654321076543210765432107654321076543210765432107654321076543210
		// .rrrrggg...gb.bbbrrrrggggbbbbDD.mmmmmmmmmmmmmmmmllllllllllllllll
		//  ^               ^           ^  ^               ^
		//  +-- c0          +-- c1      |  +-- msb         +-- lsb
		//                              +-- dist

		rgb[ 0] =   (_src[0] >> 3) & 0xf;
		rgb[ 1] = ( (_src[0] << 1) & 0xe)
				| ( (_src[1] >> 4) & 0x1)
				;
		rgb[ 2] =   (_src[1]       & 0x8)
				| ( (_src[1] << 1) & 0x6)
				|   (_src[2] >> 7)
				;

		rgb[ 8] =   (_src[2] >> 3) & 0xf;
		rgb[ 9] = ( (_src[2] << 1) & 0xe)
				|   (_src[3] >> 7)
				;
		rgb[10] = (_src[2] >> 3) & 0xf;

		rgb[ 0] = bitRangeConvert(rgb[ 0], 4, 8);
		rgb[ 1] = bitRangeConvert(rgb[ 1], 4, 8);
		rgb[ 2] = bitRangeConvert(rgb[ 2], 4, 8);
		rgb[ 8] = bitRangeConvert(rgb[ 8], 4, 8);
		rgb[ 9] = bitRangeConvert(rgb[ 9], 4, 8);
		rgb[10] = bitRangeConvert(rgb[10], 4, 8);

		uint32_t col0 = uint32_t(rgb[0]<<16) | uint32_t(rgb[1]<<8) | uint32_t(rgb[ 2]);
		uint32_t col1 = uint32_t(rgb[8]<<16) | uint32_t(rgb[9]<<8) | uint32_t(rgb[10]);
		uint8_t  dist = (_src[3] & 0x6) | (col0 >= col1);
		int32_t  mod  = s_etc2Mod[dist];

		rgb[ 4] = uint8_satadd(rgb[ 0], -mod);
		rgb[ 5] = uint8_satadd(rgb[ 1], -mod);
		rgb[ 6] = uint8_satadd(rgb[ 2], -mod);

		rgb[ 0] = uint8_satadd(rgb[ 0],  mod);
		rgb[ 1] = uint8_satadd(rgb[ 1],  mod);
		rgb[ 2] = uint8_satadd(rgb[ 2],  mod);

		rgb[12] = uint8_satadd(rgb[ 8], -mod);
		rgb[13] = uint8_satadd(rgb[ 9], -mod);
		rgb[14] = uint8_satadd(rgb[10], -mod);

		rgb[ 8] = uint8_satadd(rgb[ 8],  mod);
		rgb[ 9] = uint8_satadd(rgb[ 9],  mod);
		rgb[10] = uint8_satadd(rgb[10],  mod);

		uint32_t indexMsb = (_src[4]<<8) | _src[5];
		uint32_t indexLsb = (_src[6]<<8) | _src[7];

		for (uint32_t ii = 0; ii < 16; ++ii)
		{
			const uint32_t idx  = (ii&0xc) | ( (ii & 0x3)<<4);
			const uint32_t lsbi = indexLsb & 1;
			const uint32_t msbi = (indexMsb & 1)<<1;
			const uint32_t pal  = (lsbi | msbi)<<2;

			_dst[idx + 0] = rgb[pal+2];
			_dst[idx + 1] = rgb[pal+1];
			_dst[idx + 2] = rgb[pal+0];
			_dst[idx + 3] = 255;

			indexLsb >>= 1;
			indexMsb >>= 1;
		}
	}

	void decodeBlockEtc2ModePlanar(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		// 0       1       2       3       4       5       6       7
		// 7654321076543210765432107654321076543210765432107654321076543210
		// .rrrrrrg.ggggggb...bb.bbbrrrrr.rgggggggbbbbbbrrrrrrgggggggbbbbbb
		//  ^                       ^                   ^
		//  +-- c0                  +-- cH              +-- cV

		uint8_t c0[3];
		uint8_t cH[3];
		uint8_t cV[3];

		c0[0] =   (_src[0] >> 1) & 0x3f;
		c0[1] = ( (_src[0] & 1) << 6)
			  | ( (_src[1] >> 1) & 0x3f)
			  ;
		c0[2] = ( (_src[1] & 1) << 5)
			  | ( (_src[2] & 0x18) )
			  | ( (_src[2] << 1) & 6)
			  | ( (_src[3] >> 7) )
			  ;

		cH[0] = ( (_src[3] >> 1) & 0x3e)
			  | (_src[3] & 1)
			  ;
		cH[1] = _src[4] >> 1;
		cH[2] = ( (_src[4] & 1) << 5)
			  | (_src[5] >> 3)
			  ;

		cV[0] = ( (_src[5] & 0x7) << 3)
			  | (_src[6] >> 5)
			  ;
		cV[1] = ( (_src[6] & 0x1f) << 2)
			  | (_src[7] >> 5)
			  ;
		cV[2] = _src[7] & 0x3f;

		c0[0] = bitRangeConvert(c0[0], 6, 8);
		c0[1] = bitRangeConvert(c0[1], 7, 8);
		c0[2] = bitRangeConvert(c0[2], 6, 8);

		cH[0] = bitRangeConvert(cH[0], 6, 8);
		cH[1] = bitRangeConvert(cH[1], 7, 8);
		cH[2] = bitRangeConvert(cH[2], 6, 8);

		cV[0] = bitRangeConvert(cV[0], 6, 8);
		cV[1] = bitRangeConvert(cV[1], 7, 8);
		cV[2] = bitRangeConvert(cV[2], 6, 8);

		int16_t dy[3];
		dy[0] = cV[0] - c0[0];
		dy[1] = cV[1] - c0[1];
		dy[2] = cV[2] - c0[2];

		int16_t sx[3];
		sx[0] = int16_t(c0[0])<<2;
		sx[1] = int16_t(c0[1])<<2;
		sx[2] = int16_t(c0[2])<<2;

		int16_t ex[3];
		ex[0] = int16_t(cH[0])<<2;
		ex[1] = int16_t(cH[1])<<2;
		ex[2] = int16_t(cH[2])<<2;

		for (int32_t vv = 0; vv < 4; ++vv)
		{
			int16_t dx[3];
			dx[0] = (ex[0] - sx[0])>>2;
			dx[1] = (ex[1] - sx[1])>>2;
			dx[2] = (ex[2] - sx[2])>>2;

			for (int32_t hh = 0; hh < 4; ++hh)
			{
				const uint32_t idx  = (vv<<4) + (hh<<2);

				_dst[idx + 0] = uint8_sat( (sx[2] + dx[2]*hh)>>2);
				_dst[idx + 1] = uint8_sat( (sx[1] + dx[1]*hh)>>2);
				_dst[idx + 2] = uint8_sat( (sx[0] + dx[0]*hh)>>2);
				_dst[idx + 3] = 255;
			}

			sx[0] += dy[0];
			sx[1] += dy[1];
			sx[2] += dy[2];

			ex[0] += dy[0];
			ex[1] += dy[1];
			ex[2] += dy[2];
		}
	}

	void decodeBlockEtc12(uint8_t _dst[16*4], const uint8_t _src[8])
	{
		bool flipBit = 0 != (_src[3] & 0x1);
		bool diffBit = 0 != (_src[3] & 0x2);

		uint8_t rgb[8];

		if (diffBit)
		{
			rgb[0]  = _src[0] >> 3;
			rgb[1]  = _src[1] >> 3;
			rgb[2]  = _src[2] >> 3;

			int8_t diff[3];
			diff[0] = int8_t( (_src[0] & 0x7)<<5)>>5;
			diff[1] = int8_t( (_src[1] & 0x7)<<5)>>5;
			diff[2] = int8_t( (_src[2] & 0x7)<<5)>>5;

			int8_t rr = rgb[0] + diff[0];
			int8_t gg = rgb[1] + diff[1];
			int8_t bb = rgb[2] + diff[2];

			// Etc2 3-modes
			if (rr < 0 || rr > 31)
			{
				decodeBlockEtc2ModeT(_dst, _src);
				return;
			}
			if (gg < 0 || gg > 31)
			{
				decodeBlockEtc2ModeH(_dst, _src);
				return;
			}
			if (bb < 0 || bb > 31)
			{
				decodeBlockEtc2ModePlanar(_dst, _src);
				return;
			}

			// Etc1
			rgb[0] = bitRangeConvert(rgb[0], 5, 8);
			rgb[1] = bitRangeConvert(rgb[1], 5, 8);
			rgb[2] = bitRangeConvert(rgb[2], 5, 8);
			rgb[4] = bitRangeConvert(rr, 5, 8);
			rgb[5] = bitRangeConvert(gg, 5, 8);
			rgb[6] = bitRangeConvert(bb, 5, 8);
		}
		else
		{
			rgb[0] = _src[0] >> 4;
			rgb[1] = _src[1] >> 4;
			rgb[2] = _src[2] >> 4;

			rgb[4] = _src[0] & 0xf;
			rgb[5] = _src[1] & 0xf;
			rgb[6] = _src[2] & 0xf;

			rgb[0] = bitRangeConvert(rgb[0], 4, 8);
			rgb[1] = bitRangeConvert(rgb[1], 4, 8);
			rgb[2] = bitRangeConvert(rgb[2], 4, 8);
			rgb[4] = bitRangeConvert(rgb[4], 4, 8);
			rgb[5] = bitRangeConvert(rgb[5], 4, 8);
			rgb[6] = bitRangeConvert(rgb[6], 4, 8);
		}

		uint32_t table[2];
		table[0] = (_src[3] >> 5) & 0x7;
		table[1] = (_src[3] >> 2) & 0x7;

		uint32_t indexMsb = (_src[4]<<8) | _src[5];
		uint32_t indexLsb = (_src[6]<<8) | _src[7];

		if (flipBit)
		{
			for (uint32_t ii = 0; ii < 16; ++ii)
			{
				const uint32_t block = (ii>>1)&1;
				const uint32_t color = block<<2;
				const uint32_t idx   = (ii&0xc) | ( (ii & 0x3)<<4);
				const uint32_t lsbi  = indexLsb & 1;
				const uint32_t msbi  = (indexMsb & 1)<<1;
				const  int32_t mod   = s_etc1Mod[table[block] ][lsbi | msbi];

				_dst[idx + 0] = uint8_satadd(rgb[color+2], mod);
				_dst[idx + 1] = uint8_satadd(rgb[color+1], mod);
				_dst[idx + 2] = uint8_satadd(rgb[color+0], mod);
				_dst[idx + 3] = 255;

				indexLsb >>= 1;
				indexMsb >>= 1;
			}
		}
		else
		{
			for (uint32_t ii = 0; ii < 16; ++ii)
			{
				const uint32_t block = ii>>3;
				const uint32_t color = block<<2;
				const uint32_t idx   = (ii&0xc) | ( (ii & 0x3)<<4);
				const uint32_t lsbi  = indexLsb & 1;
				const uint32_t msbi  = (indexMsb & 1)<<1;
				const  int32_t mod   = s_etc1Mod[table[block] ][lsbi | msbi];

				_dst[idx + 0] = uint8_satadd(rgb[color+2], mod);
				_dst[idx + 1] = uint8_satadd(rgb[color+1], mod);
				_dst[idx + 2] = uint8_satadd(rgb[color+0], mod);
				_dst[idx + 3] = 255;

				indexLsb >>= 1;
				indexMsb >>= 1;
			}
		}
	}

	static const uint8_t s_pvrtcFactors[16][4] =
	{
		{  4,  4,  4,  4 },
		{  2,  6,  2,  6 },
		{  8,  0,  8,  0 },
		{  6,  2,  6,  2 },

		{  2,  2,  6,  6 },
		{  1,  3,  3,  9 },
		{  4,  0, 12,  0 },
		{  3,  1,  9,  3 },

		{  8,  8,  0,  0 },
		{  4, 12,  0,  0 },
		{ 16,  0,  0,  0 },
		{ 12,  4,  0,  0 },

		{  6,  6,  2,  2 },
		{  3,  9,  1,  3 },
		{ 12,  0,  4,  0 },
		{  9,  3,  3,  1 },
	};

	static const uint8_t s_pvrtcWeights[8][4] =
	{
		{ 8, 0, 8, 0 },
		{ 5, 3, 5, 3 },
		{ 3, 5, 3, 5 },
		{ 0, 8, 0, 8 },

		{ 8, 0, 8, 0 },
		{ 4, 4, 4, 4 },
		{ 4, 4, 0, 0 },
		{ 0, 8, 0, 8 },
	};

	uint32_t morton2d(uint32_t _x, uint32_t _y)
	{
		using namespace bx;
		const uint32_t tmpx   = uint32_part1by1(_x);
		const uint32_t xbits  = uint32_sll(tmpx, 1);
		const uint32_t ybits  = uint32_part1by1(_y);
		const uint32_t result = uint32_or(xbits, ybits);
		return result;
	}

	uint32_t getColor(const uint8_t _src[8])
	{
		return 0
			| _src[7]<<24
			| _src[6]<<16
			| _src[5]<<8
			| _src[4]
			;
	}

	void decodeBlockPtc14RgbAddA(uint32_t _block, uint32_t* _r, uint32_t* _g, uint32_t* _b, uint8_t _factor)
	{
		if (0 != (_block & (1<<15) ) )
		{
			*_r += bitRangeConvert( (_block >> 10) & 0x1f, 5, 8) * _factor;
			*_g += bitRangeConvert( (_block >>  5) & 0x1f, 5, 8) * _factor;
			*_b += bitRangeConvert( (_block >>  1) & 0x0f, 4, 8) * _factor;
		}
		else
		{
			*_r += bitRangeConvert( (_block >>  8) &  0xf, 4, 8) * _factor;
			*_g += bitRangeConvert( (_block >>  4) &  0xf, 4, 8) * _factor;
			*_b += bitRangeConvert( (_block >>  1) &  0x7, 3, 8) * _factor;
		}
	}

	void decodeBlockPtc14RgbAddB(uint32_t _block, uint32_t* _r, uint32_t* _g, uint32_t* _b, uint8_t _factor)
	{
		if (0 != (_block & (1<<31) ) )
		{
			*_r += bitRangeConvert( (_block >> 26) & 0x1f, 5, 8) * _factor;
			*_g += bitRangeConvert( (_block >> 21) & 0x1f, 5, 8) * _factor;
			*_b += bitRangeConvert( (_block >> 16) & 0x1f, 5, 8) * _factor;
		}
		else
		{
			*_r += bitRangeConvert( (_block >> 24) &  0xf, 4, 8) * _factor;
			*_g += bitRangeConvert( (_block >> 20) &  0xf, 4, 8) * _factor;
			*_b += bitRangeConvert( (_block >> 16) &  0xf, 4, 8) * _factor;
		}
	}

	void decodeBlockPtc14(uint8_t _dst[16*4], const uint8_t* _src, uint32_t _x, uint32_t _y, uint32_t _width, uint32_t _height)
	{
		// 0       1       2       3       4       5       6       7
		// 7654321076543210765432107654321076543210765432107654321076543210
		// mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmyrrrrrgggggbbbbbxrrrrrgggggbbbbp
		// ^                               ^^              ^^             ^
		// +-- modulation data             |+- B color     |+- A color    |
		//                                 +-- B opaque    +-- A opaque   |
		//                                           alpha punchthrough --+

		const uint8_t* bc = &_src[morton2d(_x, _y) * 8];

		uint32_t mod = 0
			| bc[3]<<24
			| bc[2]<<16
			| bc[1]<<8
			| bc[0]
			;

		const bool punchthrough = !!(bc[7] & 1);
		const uint8_t* weightTable = s_pvrtcWeights[4 * punchthrough];
		const uint8_t* factorTable = s_pvrtcFactors[0];

		for (int yy = 0; yy < 4; ++yy)
		{
			const uint32_t yOffset = (yy < 2) ? -1 : 0;
			const uint32_t y0 = (_y + yOffset) % _height;
			const uint32_t y1 = (y0 +       1) % _height;

			for (int xx = 0; xx < 4; ++xx)
			{
				const uint32_t xOffset = (xx < 2) ? -1 : 0;
				const uint32_t x0 = (_x + xOffset) % _width;
				const uint32_t x1 = (x0 +       1) % _width;

				const uint32_t bc0 = getColor(&_src[morton2d(x0, y0) * 8]);
				const uint32_t bc1 = getColor(&_src[morton2d(x1, y0) * 8]);
				const uint32_t bc2 = getColor(&_src[morton2d(x0, y1) * 8]);
				const uint32_t bc3 = getColor(&_src[morton2d(x1, y1) * 8]);

				const uint8_t f0 = factorTable[0];
				const uint8_t f1 = factorTable[1];
				const uint8_t f2 = factorTable[2];
				const uint8_t f3 = factorTable[3];

				uint32_t ar = 0, ag = 0, ab = 0;
				decodeBlockPtc14RgbAddA(bc0, &ar, &ag, &ab, f0);
				decodeBlockPtc14RgbAddA(bc1, &ar, &ag, &ab, f1);
				decodeBlockPtc14RgbAddA(bc2, &ar, &ag, &ab, f2);
				decodeBlockPtc14RgbAddA(bc3, &ar, &ag, &ab, f3);

				uint32_t br = 0, bg = 0, bb = 0;
				decodeBlockPtc14RgbAddB(bc0, &br, &bg, &bb, f0);
				decodeBlockPtc14RgbAddB(bc1, &br, &bg, &bb, f1);
				decodeBlockPtc14RgbAddB(bc2, &br, &bg, &bb, f2);
				decodeBlockPtc14RgbAddB(bc3, &br, &bg, &bb, f3);

				const uint8_t* weight = &weightTable[(mod & 3)*4];
				const uint8_t wa = weight[0];
				const uint8_t wb = weight[1];

				_dst[(yy*4 + xx)*4+0] = uint8_t( (ab * wa + bb * wb) >> 7);
				_dst[(yy*4 + xx)*4+1] = uint8_t( (ag * wa + bg * wb) >> 7);
				_dst[(yy*4 + xx)*4+2] = uint8_t( (ar * wa + br * wb) >> 7);
				_dst[(yy*4 + xx)*4+3] = 255;

				mod >>= 2;
				factorTable += 4;
			}
		}
	}

	void decodeBlockPtc14ARgbaAddA(uint32_t _block, uint32_t* _r, uint32_t* _g, uint32_t* _b, uint32_t* _a, uint8_t _factor)
	{
		if (0 != (_block & (1<<15) ) )
		{
			*_r += bitRangeConvert( (_block >> 10) & 0x1f, 5, 8) * _factor;
			*_g += bitRangeConvert( (_block >>  5) & 0x1f, 5, 8) * _factor;
			*_b += bitRangeConvert( (_block >>  1) & 0x0f, 4, 8) * _factor;
			*_a += 255;
		}
		else
		{
			*_r += bitRangeConvert( (_block >>  8) &  0xf, 4, 8) * _factor;
			*_g += bitRangeConvert( (_block >>  4) &  0xf, 4, 8) * _factor;
			*_b += bitRangeConvert( (_block >>  1) &  0x7, 3, 8) * _factor;
			*_a += bitRangeConvert( (_block >> 12) &  0x7, 3, 8) * _factor;
		}
	}

	void decodeBlockPtc14ARgbaAddB(uint32_t _block, uint32_t* _r, uint32_t* _g, uint32_t* _b, uint32_t* _a, uint8_t _factor)
	{
		if (0 != (_block & (1<<31) ) )
		{
			*_r += bitRangeConvert( (_block >> 26) & 0x1f, 5, 8) * _factor;
			*_g += bitRangeConvert( (_block >> 21) & 0x1f, 5, 8) * _factor;
			*_b += bitRangeConvert( (_block >> 16) & 0x1f, 5, 8) * _factor;
			*_a += 255;
		}
		else
		{
			*_r += bitRangeConvert( (_block >> 24) &  0xf, 4, 8) * _factor;
			*_g += bitRangeConvert( (_block >> 20) &  0xf, 4, 8) * _factor;
			*_b += bitRangeConvert( (_block >> 16) &  0xf, 4, 8) * _factor;
			*_a += bitRangeConvert( (_block >> 28) &  0x7, 3, 8) * _factor;
		}
	}

	void decodeBlockPtc14A(uint8_t _dst[16*4], const uint8_t* _src, uint32_t _x, uint32_t _y, uint32_t _width, uint32_t _height)
	{
		// 0       1       2       3       4       5       6       7
		// 7654321076543210765432107654321076543210765432107654321076543210
		// mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmyrrrrrgggggbbbbbxrrrrrgggggbbbbp
		// ^                               ^^              ^^             ^
		// +-- modulation data             |+- B color     |+- A color    |
		//                                 +-- B opaque    +-- A opaque   |
		//                                           alpha punchthrough --+

		const uint8_t* bc = &_src[morton2d(_x, _y) * 8];

		uint32_t mod = 0
			| bc[3]<<24
			| bc[2]<<16
			| bc[1]<<8
			| bc[0]
			;

		const bool punchthrough = !!(bc[7] & 1);
		const uint8_t* weightTable = s_pvrtcWeights[4 * punchthrough];
		const uint8_t* factorTable = s_pvrtcFactors[0];

		for (int yy = 0; yy < 4; ++yy)
		{
			const uint32_t yOffset = (yy < 2) ? -1 : 0;
			const uint32_t y0 = (_y + yOffset) % _height;
			const uint32_t y1 = (y0 +       1) % _height;

			for (int xx = 0; xx < 4; ++xx)
			{
				const uint32_t xOffset = (xx < 2) ? -1 : 0;
				const uint32_t x0 = (_x + xOffset) % _width;
				const uint32_t x1 = (x0 +       1) % _width;

				const uint32_t bc0 = getColor(&_src[morton2d(x0, y0) * 8]);
				const uint32_t bc1 = getColor(&_src[morton2d(x1, y0) * 8]);
				const uint32_t bc2 = getColor(&_src[morton2d(x0, y1) * 8]);
				const uint32_t bc3 = getColor(&_src[morton2d(x1, y1) * 8]);

				const uint8_t f0 = factorTable[0];
				const uint8_t f1 = factorTable[1];
				const uint8_t f2 = factorTable[2];
				const uint8_t f3 = factorTable[3];

				uint32_t ar = 0, ag = 0, ab = 0, aa = 0;
				decodeBlockPtc14ARgbaAddA(bc0, &ar, &ag, &ab, &aa, f0);
				decodeBlockPtc14ARgbaAddA(bc1, &ar, &ag, &ab, &aa, f1);
				decodeBlockPtc14ARgbaAddA(bc2, &ar, &ag, &ab, &aa, f2);
				decodeBlockPtc14ARgbaAddA(bc3, &ar, &ag, &ab, &aa, f3);

				uint32_t br = 0, bg = 0, bb = 0, ba = 0;
				decodeBlockPtc14ARgbaAddB(bc0, &br, &bg, &bb, &ba, f0);
				decodeBlockPtc14ARgbaAddB(bc1, &br, &bg, &bb, &ba, f1);
				decodeBlockPtc14ARgbaAddB(bc2, &br, &bg, &bb, &ba, f2);
				decodeBlockPtc14ARgbaAddB(bc3, &br, &bg, &bb, &ba, f3);

				const uint8_t* weight = &weightTable[(mod & 3)*4];
				const uint8_t wa = weight[0];
				const uint8_t wb = weight[1];
				const uint8_t wc = weight[2];
				const uint8_t wd = weight[3];

				_dst[(yy*4 + xx)*4+0] = uint8_t( (ab * wa + bb * wb) >> 7);
				_dst[(yy*4 + xx)*4+1] = uint8_t( (ag * wa + bg * wb) >> 7);
				_dst[(yy*4 + xx)*4+2] = uint8_t( (ar * wa + br * wb) >> 7);
				_dst[(yy*4 + xx)*4+3] = uint8_t( (aa * wc + ba * wd) >> 7);

				mod >>= 2;
				factorTable += 4;
			}
		}
	}

// DDS
#define DDS_MAGIC             BX_MAKEFOURCC('D', 'D', 'S', ' ')
#define DDS_HEADER_SIZE       124

#define DDS_DXT1 BX_MAKEFOURCC('D', 'X', 'T', '1')
#define DDS_DXT2 BX_MAKEFOURCC('D', 'X', 'T', '2')
#define DDS_DXT3 BX_MAKEFOURCC('D', 'X', 'T', '3')
#define DDS_DXT4 BX_MAKEFOURCC('D', 'X', 'T', '4')
#define DDS_DXT5 BX_MAKEFOURCC('D', 'X', 'T', '5')
#define DDS_ATI1 BX_MAKEFOURCC('A', 'T', 'I', '1')
#define DDS_BC4U BX_MAKEFOURCC('B', 'C', '4', 'U')
#define DDS_ATI2 BX_MAKEFOURCC('A', 'T', 'I', '2')
#define DDS_BC5U BX_MAKEFOURCC('B', 'C', '5', 'U')
#define DDS_DX10 BX_MAKEFOURCC('D', 'X', '1', '0')

#define DDS_A8R8G8B8       21
#define DDS_R5G6B5         23
#define DDS_A1R5G5B5       25
#define DDS_A4R4G4B4       26
#define DDS_A2B10G10R10    31
#define DDS_G16R16         34
#define DDS_A2R10G10B10    35
#define DDS_A16B16G16R16   36
#define DDS_A8L8           51
#define DDS_R16F           111
#define DDS_G16R16F        112
#define DDS_A16B16G16R16F  113
#define DDS_R32F           114
#define DDS_G32R32F        115
#define DDS_A32B32G32R32F  116

#define DDS_FORMAT_R32G32B32A32_FLOAT  2
#define DDS_FORMAT_R32G32B32A32_UINT   3
#define DDS_FORMAT_R16G16B16A16_FLOAT  10
#define DDS_FORMAT_R16G16B16A16_UNORM  11
#define DDS_FORMAT_R16G16B16A16_UINT   12
#define DDS_FORMAT_R32G32_FLOAT        16
#define DDS_FORMAT_R32G32_UINT         17
#define DDS_FORMAT_R10G10B10A2_UNORM   24
#define DDS_FORMAT_R11G11B10_FLOAT     26
#define DDS_FORMAT_R8G8B8A8_UNORM      28
#define DDS_FORMAT_R8G8B8A8_UNORM_SRGB 29
#define DDS_FORMAT_R16G16_FLOAT        34
#define DDS_FORMAT_R16G16_UNORM        35
#define DDS_FORMAT_R32_FLOAT           41
#define DDS_FORMAT_R32_UINT            42
#define DDS_FORMAT_R8G8_UNORM          49
#define DDS_FORMAT_R16_FLOAT           54
#define DDS_FORMAT_R16_UNORM           56
#define DDS_FORMAT_R8_UNORM            61
#define DDS_FORMAT_R1_UNORM            66
#define DDS_FORMAT_BC1_UNORM           71
#define DDS_FORMAT_BC1_UNORM_SRGB      72
#define DDS_FORMAT_BC2_UNORM           74
#define DDS_FORMAT_BC2_UNORM_SRGB      75
#define DDS_FORMAT_BC3_UNORM           77
#define DDS_FORMAT_BC3_UNORM_SRGB      78
#define DDS_FORMAT_BC4_UNORM           80
#define DDS_FORMAT_BC5_UNORM           83
#define DDS_FORMAT_B5G6R5_UNORM        85
#define DDS_FORMAT_B5G5R5A1_UNORM      86
#define DDS_FORMAT_B8G8R8A8_UNORM      87
#define DDS_FORMAT_B8G8R8A8_UNORM_SRGB 91
#define DDS_FORMAT_BC6H_SF16           96
#define DDS_FORMAT_BC7_UNORM           98
#define DDS_FORMAT_BC7_UNORM_SRGB      99
#define DDS_FORMAT_B4G4R4A4_UNORM      115

#define DDSD_CAPS                   0x00000001
#define DDSD_HEIGHT                 0x00000002
#define DDSD_WIDTH                  0x00000004
#define DDSD_PITCH                  0x00000008
#define DDSD_PIXELFORMAT            0x00001000
#define DDSD_MIPMAPCOUNT            0x00020000
#define DDSD_LINEARSIZE             0x00080000
#define DDSD_DEPTH                  0x00800000

#define DDPF_ALPHAPIXELS            0x00000001
#define DDPF_ALPHA                  0x00000002
#define DDPF_FOURCC                 0x00000004
#define DDPF_INDEXED                0x00000020
#define DDPF_RGB                    0x00000040
#define DDPF_YUV                    0x00000200
#define DDPF_LUMINANCE              0x00020000

#define DDSCAPS_COMPLEX             0x00000008
#define DDSCAPS_TEXTURE             0x00001000
#define DDSCAPS_MIPMAP              0x00400000

#define DDSCAPS2_CUBEMAP            0x00000200
#define DDSCAPS2_CUBEMAP_POSITIVEX  0x00000400
#define DDSCAPS2_CUBEMAP_NEGATIVEX  0x00000800
#define DDSCAPS2_CUBEMAP_POSITIVEY  0x00001000
#define DDSCAPS2_CUBEMAP_NEGATIVEY  0x00002000
#define DDSCAPS2_CUBEMAP_POSITIVEZ  0x00004000
#define DDSCAPS2_CUBEMAP_NEGATIVEZ  0x00008000

#define DDS_CUBEMAP_ALLFACES (DDSCAPS2_CUBEMAP_POSITIVEX|DDSCAPS2_CUBEMAP_NEGATIVEX \
							 |DDSCAPS2_CUBEMAP_POSITIVEY|DDSCAPS2_CUBEMAP_NEGATIVEY \
							 |DDSCAPS2_CUBEMAP_POSITIVEZ|DDSCAPS2_CUBEMAP_NEGATIVEZ)

#define DDSCAPS2_VOLUME             0x00200000

	struct TranslateDdsFormat
	{
		uint32_t m_format;
		TextureFormat::Enum m_textureFormat;
		bool m_srgb;
	};

	static TranslateDdsFormat s_translateDdsFourccFormat[] =
	{
		{ DDS_DXT1,                  TextureFormat::BC1,     false },
		{ DDS_DXT2,                  TextureFormat::BC2,     false },
		{ DDS_DXT3,                  TextureFormat::BC2,     false },
		{ DDS_DXT4,                  TextureFormat::BC3,     false },
		{ DDS_DXT5,                  TextureFormat::BC3,     false },
		{ DDS_ATI1,                  TextureFormat::BC4,     false },
		{ DDS_BC4U,                  TextureFormat::BC4,     false },
		{ DDS_ATI2,                  TextureFormat::BC5,     false },
		{ DDS_BC5U,                  TextureFormat::BC5,     false },
		{ DDS_A16B16G16R16,          TextureFormat::RGBA16,  false },
		{ DDS_A16B16G16R16F,         TextureFormat::RGBA16F, false },
		{ DDPF_RGB|DDPF_ALPHAPIXELS, TextureFormat::BGRA8,   false },
		{ DDPF_INDEXED,              TextureFormat::R8,      false },
		{ DDPF_LUMINANCE,            TextureFormat::R8,      false },
		{ DDPF_ALPHA,                TextureFormat::R8,      false },
		{ DDS_R16F,                  TextureFormat::R16F,    false },
		{ DDS_R32F,                  TextureFormat::R32F,    false },
		{ DDS_A8L8,                  TextureFormat::RG8,     false },
		{ DDS_G16R16,                TextureFormat::RG16,    false },
		{ DDS_G16R16F,               TextureFormat::RG16F,   false },
		{ DDS_G32R32F,               TextureFormat::RG32F,   false },
		{ DDS_A8R8G8B8,              TextureFormat::BGRA8,   false },
		{ DDS_A16B16G16R16,          TextureFormat::RGBA16,  false },
		{ DDS_A16B16G16R16F,         TextureFormat::RGBA16F, false },
		{ DDS_A32B32G32R32F,         TextureFormat::RGBA32F, false },
		{ DDS_R5G6B5,                TextureFormat::R5G6B5,  false },
		{ DDS_A4R4G4B4,              TextureFormat::RGBA4,   false },
		{ DDS_A1R5G5B5,              TextureFormat::RGB5A1,  false },
		{ DDS_A2B10G10R10,           TextureFormat::RGB10A2, false },
	};

	static TranslateDdsFormat s_translateDxgiFormat[] =
	{
		{ DDS_FORMAT_BC1_UNORM,           TextureFormat::BC1,        false },
		{ DDS_FORMAT_BC1_UNORM_SRGB,      TextureFormat::BC1,        true  },
		{ DDS_FORMAT_BC2_UNORM,           TextureFormat::BC2,        false },
		{ DDS_FORMAT_BC2_UNORM_SRGB,      TextureFormat::BC2,        true  },
		{ DDS_FORMAT_BC3_UNORM,           TextureFormat::BC3,        false },
		{ DDS_FORMAT_BC3_UNORM_SRGB,      TextureFormat::BC3,        true  },
		{ DDS_FORMAT_BC4_UNORM,           TextureFormat::BC4,        false },
		{ DDS_FORMAT_BC5_UNORM,           TextureFormat::BC5,        false },
		{ DDS_FORMAT_BC6H_SF16,           TextureFormat::BC6H,       false },
		{ DDS_FORMAT_BC7_UNORM,           TextureFormat::BC7,        false },
		{ DDS_FORMAT_BC7_UNORM_SRGB,      TextureFormat::BC7,        true  },

		{ DDS_FORMAT_R1_UNORM,            TextureFormat::R1,         false },
		{ DDS_FORMAT_R8_UNORM,            TextureFormat::R8,         false },
		{ DDS_FORMAT_R16_UNORM,           TextureFormat::R16,        false },
		{ DDS_FORMAT_R16_FLOAT,           TextureFormat::R16F,       false },
		{ DDS_FORMAT_R32_UINT,            TextureFormat::R32U,       false },
		{ DDS_FORMAT_R32_FLOAT,           TextureFormat::R32F,       false },
		{ DDS_FORMAT_R8G8_UNORM,          TextureFormat::RG8,        false },
		{ DDS_FORMAT_R16G16_UNORM,        TextureFormat::RG16,       false },
		{ DDS_FORMAT_R16G16_FLOAT,        TextureFormat::RG16F,      false },
		{ DDS_FORMAT_R32G32_UINT,         TextureFormat::RG32U,      false },
		{ DDS_FORMAT_R32G32_FLOAT,        TextureFormat::RG32F,      false },
		{ DDS_FORMAT_B8G8R8A8_UNORM,      TextureFormat::BGRA8,      false },
		{ DDS_FORMAT_B8G8R8A8_UNORM_SRGB, TextureFormat::BGRA8,      true  },
		{ DDS_FORMAT_R8G8B8A8_UNORM,      TextureFormat::RGBA8,      false },
		{ DDS_FORMAT_R8G8B8A8_UNORM_SRGB, TextureFormat::RGBA8,      true  },
		{ DDS_FORMAT_R16G16B16A16_UNORM,  TextureFormat::RGBA16,     false },
		{ DDS_FORMAT_R16G16B16A16_FLOAT,  TextureFormat::RGBA16F,    false },
		{ DDS_FORMAT_R32G32B32A32_UINT,   TextureFormat::RGBA32U,    false },
		{ DDS_FORMAT_R32G32B32A32_FLOAT,  TextureFormat::RGBA32F,    false },
		{ DDS_FORMAT_B5G6R5_UNORM,        TextureFormat::R5G6B5,     false },
		{ DDS_FORMAT_B4G4R4A4_UNORM,      TextureFormat::RGBA4,      false },
		{ DDS_FORMAT_B5G5R5A1_UNORM,      TextureFormat::RGB5A1,     false },
		{ DDS_FORMAT_R10G10B10A2_UNORM,   TextureFormat::RGB10A2,    false },
		{ DDS_FORMAT_R11G11B10_FLOAT,     TextureFormat::R11G11B10F, false },
	};

	struct TranslateDdsPixelFormat
	{
		uint32_t m_bitCount;
		uint32_t m_bitmask[4];
		TextureFormat::Enum m_textureFormat;
	};

	static TranslateDdsPixelFormat s_translateDdsPixelFormat[] =
	{
		{  8, { 0x000000ff, 0x00000000, 0x00000000, 0x00000000 }, TextureFormat::R8      },
		{ 16, { 0x0000ffff, 0x00000000, 0x00000000, 0x00000000 }, TextureFormat::R16U    },
		{ 16, { 0x00000f00, 0x000000f0, 0x0000000f, 0x0000f000 }, TextureFormat::RGBA4   },
		{ 16, { 0x0000f800, 0x000007e0, 0x0000001f, 0x00000000 }, TextureFormat::R5G6B5  },
		{ 16, { 0x00007c00, 0x000003e0, 0x0000001f, 0x00008000 }, TextureFormat::RGB5A1  },
		{ 32, { 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000 }, TextureFormat::BGRA8   },
		{ 32, { 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000 }, TextureFormat::BGRA8   },
		{ 32, { 0x000003ff, 0x000ffc00, 0x3ff00000, 0xc0000000 }, TextureFormat::RGB10A2 },
		{ 32, { 0x0000ffff, 0xffff0000, 0x00000000, 0x00000000 }, TextureFormat::RG16    },
		{ 32, { 0xffffffff, 0x00000000, 0x00000000, 0x00000000 }, TextureFormat::R32U    },
	};

	bool imageParseDds(ImageContainer& _imageContainer, bx::ReaderSeekerI* _reader)
	{
		uint32_t headerSize;
		bx::read(_reader, headerSize);

		if (headerSize < DDS_HEADER_SIZE)
		{
			return false;
		}

		uint32_t flags;
		bx::read(_reader, flags);

		if ( (flags & (DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT) ) != (DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT) )
		{
			return false;
		}

		uint32_t height;
		bx::read(_reader, height);

		uint32_t width;
		bx::read(_reader, width);

		uint32_t pitch;
		bx::read(_reader, pitch);

		uint32_t depth;
		bx::read(_reader, depth);

		uint32_t mips;
		bx::read(_reader, mips);

		bx::skip(_reader, 44); // reserved

		uint32_t pixelFormatSize;
		bx::read(_reader, pixelFormatSize);

		uint32_t pixelFlags;
		bx::read(_reader, pixelFlags);

		uint32_t fourcc;
		bx::read(_reader, fourcc);

		uint32_t bitCount;
		bx::read(_reader, bitCount);

		uint32_t bitmask[4];
		bx::read(_reader, bitmask, sizeof(bitmask) );

		uint32_t caps[4];
		bx::read(_reader, caps);

		bx::skip(_reader, 4); // reserved

		uint32_t dxgiFormat = 0;
		if (DDPF_FOURCC == pixelFlags
		&&  DDS_DX10 == fourcc)
		{
			bx::read(_reader, dxgiFormat);

			uint32_t dims;
			bx::read(_reader, dims);

			uint32_t miscFlags;
			bx::read(_reader, miscFlags);

			uint32_t arraySize;
			bx::read(_reader, arraySize);

			uint32_t miscFlags2;
			bx::read(_reader, miscFlags2);
		}

		if ( (caps[0] & DDSCAPS_TEXTURE) == 0)
		{
			return false;
		}

		bool cubeMap = 0 != (caps[1] & DDSCAPS2_CUBEMAP);
		if (cubeMap)
		{
			if ( (caps[1] & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES)
			{
				// parital cube map is not supported.
				return false;
			}
		}

		TextureFormat::Enum format = TextureFormat::Unknown;
		bool hasAlpha = pixelFlags & DDPF_ALPHAPIXELS;
		bool srgb = false;

		if (dxgiFormat == 0)
		{
			if (DDPF_FOURCC == (pixelFlags & DDPF_FOURCC) )
			{
				for (uint32_t ii = 0; ii < BX_COUNTOF(s_translateDdsFourccFormat); ++ii)
				{
					if (s_translateDdsFourccFormat[ii].m_format == fourcc)
					{
						format = s_translateDdsFourccFormat[ii].m_textureFormat;
						break;
					}
				}
			}
			else
			{
				for (uint32_t ii = 0; ii < BX_COUNTOF(s_translateDdsPixelFormat); ++ii)
				{
					const TranslateDdsPixelFormat& pf = s_translateDdsPixelFormat[ii];
					if (pf.m_bitCount   == bitCount
					&&  pf.m_bitmask[0] == bitmask[0]
					&&  pf.m_bitmask[1] == bitmask[1]
					&&  pf.m_bitmask[2] == bitmask[2]
					&&  pf.m_bitmask[3] == bitmask[3])
					{
						format = pf.m_textureFormat;
						break;
					}
				}
			}
		}
		else
		{
			for (uint32_t ii = 0; ii < BX_COUNTOF(s_translateDxgiFormat); ++ii)
			{
				if (s_translateDxgiFormat[ii].m_format == dxgiFormat)
				{
					format = s_translateDxgiFormat[ii].m_textureFormat;
					srgb = s_translateDxgiFormat[ii].m_srgb;
					break;
				}
			}
		}

		_imageContainer.m_data = NULL;
		_imageContainer.m_size = 0;
		_imageContainer.m_offset = (uint32_t)bx::seek(_reader);
		_imageContainer.m_width  = width;
		_imageContainer.m_height = height;
		_imageContainer.m_depth  = depth;
		_imageContainer.m_format   = uint8_t(format);
		_imageContainer.m_numMips  = uint8_t( (caps[0] & DDSCAPS_MIPMAP) ? mips : 1);
		_imageContainer.m_hasAlpha = hasAlpha;
		_imageContainer.m_cubeMap  = cubeMap;
		_imageContainer.m_ktx = false;
		_imageContainer.m_srgb = srgb;

		return TextureFormat::Unknown != format;
	}

// KTX
#define KTX_MAGIC       BX_MAKEFOURCC(0xAB, 'K', 'T', 'X')
#define KTX_HEADER_SIZE 64

#define KTX_ETC1_RGB8_OES                             0x8D64
#define KTX_COMPRESSED_R11_EAC                        0x9270
#define KTX_COMPRESSED_SIGNED_R11_EAC                 0x9271
#define KTX_COMPRESSED_RG11_EAC                       0x9272
#define KTX_COMPRESSED_SIGNED_RG11_EAC                0x9273
#define KTX_COMPRESSED_RGB8_ETC2                      0x9274
#define KTX_COMPRESSED_SRGB8_ETC2                     0x9275
#define KTX_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2  0x9276
#define KTX_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9277
#define KTX_COMPRESSED_RGBA8_ETC2_EAC                 0x9278
#define KTX_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC          0x9279
#define KTX_COMPRESSED_RGB_PVRTC_4BPPV1_IMG           0x8C00
#define KTX_COMPRESSED_RGB_PVRTC_2BPPV1_IMG           0x8C01
#define KTX_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG          0x8C02
#define KTX_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG          0x8C03
#define KTX_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG          0x9137
#define KTX_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG          0x9138
#define KTX_COMPRESSED_RGBA_S3TC_DXT1_EXT             0x83F1
#define KTX_COMPRESSED_RGBA_S3TC_DXT3_EXT             0x83F2
#define KTX_COMPRESSED_RGBA_S3TC_DXT5_EXT             0x83F3
#define KTX_COMPRESSED_LUMINANCE_LATC1_EXT            0x8C70
#define KTX_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT      0x8C72
#define KTX_COMPRESSED_RGBA_BPTC_UNORM_ARB            0x8E8C
#define KTX_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB      0x8E8D
#define KTX_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB      0x8E8E
#define KTX_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB    0x8E8F
#define KTX_R8                                        0x8229
#define KTX_R16                                       0x822A
#define KTX_RG8                                       0x822B
#define KTX_RG16                                      0x822C
#define KTX_R16F                                      0x822D
#define KTX_R32F                                      0x822E
#define KTX_RG16F                                     0x822F
#define KTX_RG32F                                     0x8230
#define KTX_RGBA16                                    0x805B
#define KTX_RGBA16F                                   0x881A
#define KTX_R32UI                                     0x8236
#define KTX_RG32UI                                    0x823C
#define KTX_RGBA32UI                                  0x8D70
#define KTX_BGRA                                      0x80E1
#define KTX_RGBA32F                                   0x8814
#define KTX_RGB565                                    0x8D62
#define KTX_RGBA4                                     0x8056
#define KTX_RGB5_A1                                   0x8057
#define KTX_RGB10_A2                                  0x8059

	static struct TranslateKtxFormat
	{
		uint32_t m_format;
		TextureFormat::Enum m_textureFormat;

	} s_translateKtxFormat[] =
	{
		{ KTX_COMPRESSED_RGBA_S3TC_DXT1_EXT,             TextureFormat::BC1     },
		{ KTX_COMPRESSED_RGBA_S3TC_DXT3_EXT,             TextureFormat::BC2     },
		{ KTX_COMPRESSED_RGBA_S3TC_DXT5_EXT,             TextureFormat::BC3     },
		{ KTX_COMPRESSED_LUMINANCE_LATC1_EXT,            TextureFormat::BC4     },
		{ KTX_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT,      TextureFormat::BC5     },
		{ KTX_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB,      TextureFormat::BC6H    },
		{ KTX_COMPRESSED_RGBA_BPTC_UNORM_ARB,            TextureFormat::BC7     },
		{ KTX_ETC1_RGB8_OES,                             TextureFormat::ETC1    },
		{ KTX_COMPRESSED_RGB8_ETC2,                      TextureFormat::ETC2    },
		{ KTX_COMPRESSED_RGBA8_ETC2_EAC,                 TextureFormat::ETC2A   },
		{ KTX_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2,  TextureFormat::ETC2A1  },
		{ KTX_COMPRESSED_RGB_PVRTC_2BPPV1_IMG,           TextureFormat::PTC12   },
		{ KTX_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG,          TextureFormat::PTC12A  },
		{ KTX_COMPRESSED_RGB_PVRTC_4BPPV1_IMG,           TextureFormat::PTC14   },
		{ KTX_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,          TextureFormat::PTC14A  },
		{ KTX_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG,          TextureFormat::PTC22   },
		{ KTX_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG,          TextureFormat::PTC24   },
		{ KTX_R8,                                        TextureFormat::R8      },
		{ KTX_RGBA16,                                    TextureFormat::RGBA16  },
		{ KTX_RGBA16F,                                   TextureFormat::RGBA16F },
		{ KTX_R32UI,                                     TextureFormat::R32U    },
		{ KTX_R32F,                                      TextureFormat::R32F    },
		{ KTX_RG8,                                       TextureFormat::RG8     },
		{ KTX_RG16,                                      TextureFormat::RG16    },
		{ KTX_RG16F,                                     TextureFormat::RG16F   },
		{ KTX_RG32UI,                                    TextureFormat::RG32U   },
		{ KTX_RG32F,                                     TextureFormat::RG32F   },
		{ KTX_BGRA,                                      TextureFormat::BGRA8   },
		{ KTX_RGBA16,                                    TextureFormat::RGBA16  },
		{ KTX_RGBA16F,                                   TextureFormat::RGBA16F },
		{ KTX_RGBA32UI,                                  TextureFormat::RGBA32U },
		{ KTX_RGBA32F,                                   TextureFormat::RGBA32F },
		{ KTX_RGB565,                                    TextureFormat::R5G6B5  },
		{ KTX_RGBA4,                                     TextureFormat::RGBA4   },
		{ KTX_RGB5_A1,                                   TextureFormat::RGB5A1  },
		{ KTX_RGB10_A2,                                  TextureFormat::RGB10A2 },
	};

	bool imageParseKtx(ImageContainer& _imageContainer, bx::ReaderSeekerI* _reader)
	{
		uint8_t identifier[8];
		bx::read(_reader, identifier);

		if (identifier[1] != '1'
		&&  identifier[2] != '1')
		{
			return false;
		}

		uint32_t endianness;
		bx::read(_reader, endianness);

		bool fromLittleEndian = 0x04030201 == endianness;

		uint32_t glType;
		bx::readHE(_reader, glType, fromLittleEndian);

		uint32_t glTypeSize;
		bx::readHE(_reader, glTypeSize, fromLittleEndian);

		uint32_t glFormat;
		bx::readHE(_reader, glFormat, fromLittleEndian);

		uint32_t glInternalFormat;
		bx::readHE(_reader, glInternalFormat, fromLittleEndian);

		uint32_t glBaseInternalFormat;
		bx::readHE(_reader, glBaseInternalFormat, fromLittleEndian);

		uint32_t width;
		bx::readHE(_reader, width, fromLittleEndian);

		uint32_t height;
		bx::readHE(_reader, height, fromLittleEndian);

		uint32_t depth;
		bx::readHE(_reader, depth, fromLittleEndian);

		uint32_t numberOfArrayElements;
		bx::readHE(_reader, numberOfArrayElements, fromLittleEndian);

		uint32_t numFaces;
		bx::readHE(_reader, numFaces, fromLittleEndian);

		uint32_t numMips;
		bx::readHE(_reader, numMips, fromLittleEndian);

		uint32_t metaDataSize;
		bx::readHE(_reader, metaDataSize, fromLittleEndian);

		// skip meta garbage...
		int64_t offset = bx::skip(_reader, metaDataSize);

		TextureFormat::Enum format = TextureFormat::Unknown;
		bool hasAlpha = false;

		for (uint32_t ii = 0; ii < BX_COUNTOF(s_translateKtxFormat); ++ii)
		{
			if (s_translateKtxFormat[ii].m_format == glInternalFormat)
			{
				format = s_translateKtxFormat[ii].m_textureFormat;
				break;
			}
		}

		_imageContainer.m_data = NULL;
		_imageContainer.m_size = 0;
		_imageContainer.m_offset = (uint32_t)offset;
		_imageContainer.m_width  = width;
		_imageContainer.m_height = height;
		_imageContainer.m_depth  = depth;
		_imageContainer.m_format = uint8_t(format);
		_imageContainer.m_numMips  = uint8_t(numMips);
		_imageContainer.m_hasAlpha = hasAlpha;
		_imageContainer.m_cubeMap  = numFaces > 1;
		_imageContainer.m_ktx = true;

		return TextureFormat::Unknown != format;
	}

// PVR3
#define PVR3_MAKE8CC(_a, _b, _c, _d, _e, _f, _g, _h) (uint64_t(BX_MAKEFOURCC(_a, _b, _c, _d) ) | (uint64_t(BX_MAKEFOURCC(_e, _f, _g, _h) )<<32) )

#define PVR3_MAGIC            BX_MAKEFOURCC('P', 'V', 'R', 3)
#define PVR3_HEADER_SIZE      52

#define PVR3_PVRTC1_2BPP_RGB  0
#define PVR3_PVRTC1_2BPP_RGBA 1
#define PVR3_PVRTC1_4BPP_RGB  2
#define PVR3_PVRTC1_4BPP_RGBA 3
#define PVR3_PVRTC2_2BPP_RGBA 4
#define PVR3_PVRTC2_4BPP_RGBA 5
#define PVR3_ETC1             6
#define PVR3_DXT1             7
#define PVR3_DXT2             8
#define PVR3_DXT3             9
#define PVR3_DXT4             10
#define PVR3_DXT5             11
#define PVR3_BC4              12
#define PVR3_BC5              13
#define PVR3_R8               PVR3_MAKE8CC('r',   0,   0,   0,  8,  0,  0,  0)
#define PVR3_R16              PVR3_MAKE8CC('r',   0,   0,   0, 16,  0,  0,  0)
#define PVR3_R32              PVR3_MAKE8CC('r',   0,   0,   0, 32,  0,  0,  0)
#define PVR3_RG8              PVR3_MAKE8CC('r', 'g',   0,   0,  8,  8,  0,  0)
#define PVR3_RG16             PVR3_MAKE8CC('r', 'g',   0,   0, 16, 16,  0,  0)
#define PVR3_RG32             PVR3_MAKE8CC('r', 'g',   0,   0, 32, 32,  0,  0)
#define PVR3_BGRA8            PVR3_MAKE8CC('b', 'g', 'r', 'a',  8,  8,  8,  8)
#define PVR3_RGBA16           PVR3_MAKE8CC('r', 'g', 'b', 'a', 16, 16, 16, 16)
#define PVR3_RGBA32           PVR3_MAKE8CC('r', 'g', 'b', 'a', 32, 32, 32, 32)
#define PVR3_RGB565           PVR3_MAKE8CC('r', 'g', 'b',   0,  5,  6,  5,  0)
#define PVR3_RGBA4            PVR3_MAKE8CC('r', 'g', 'b', 'a',  4,  4,  4,  4)
#define PVR3_RGBA51           PVR3_MAKE8CC('r', 'g', 'b', 'a',  5,  5,  5,  1)
#define PVR3_RGB10A2          PVR3_MAKE8CC('r', 'g', 'b', 'a', 10, 10, 10,  2)

#define PVR3_CHANNEL_TYPE_ANY   UINT32_MAX
#define PVR3_CHANNEL_TYPE_FLOAT UINT32_C(12)

	static struct TranslatePvr3Format
	{
		uint64_t m_format;
		uint32_t m_channelTypeMask;
		TextureFormat::Enum m_textureFormat;

	} s_translatePvr3Format[] =
	{
		{ PVR3_PVRTC1_2BPP_RGB,  PVR3_CHANNEL_TYPE_ANY,   TextureFormat::PTC12   },
		{ PVR3_PVRTC1_2BPP_RGBA, PVR3_CHANNEL_TYPE_ANY,   TextureFormat::PTC12A  },
		{ PVR3_PVRTC1_4BPP_RGB,  PVR3_CHANNEL_TYPE_ANY,   TextureFormat::PTC14   },
		{ PVR3_PVRTC1_4BPP_RGBA, PVR3_CHANNEL_TYPE_ANY,   TextureFormat::PTC14A  },
		{ PVR3_PVRTC2_2BPP_RGBA, PVR3_CHANNEL_TYPE_ANY,   TextureFormat::PTC22   },
		{ PVR3_PVRTC2_4BPP_RGBA, PVR3_CHANNEL_TYPE_ANY,   TextureFormat::PTC24   },
		{ PVR3_ETC1,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::ETC1    },
		{ PVR3_DXT1,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC1     },
		{ PVR3_DXT2,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC2     },
		{ PVR3_DXT3,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC2     },
		{ PVR3_DXT4,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC3     },
		{ PVR3_DXT5,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC3     },
		{ PVR3_BC4,              PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC4     },
		{ PVR3_BC5,              PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BC5     },
		{ PVR3_R8,               PVR3_CHANNEL_TYPE_ANY,   TextureFormat::R8      },
		{ PVR3_R16,              PVR3_CHANNEL_TYPE_ANY,   TextureFormat::R16U    },
		{ PVR3_R16,              PVR3_CHANNEL_TYPE_FLOAT, TextureFormat::R16F    },
		{ PVR3_R32,              PVR3_CHANNEL_TYPE_ANY,   TextureFormat::R32U    },
		{ PVR3_R32,              PVR3_CHANNEL_TYPE_FLOAT, TextureFormat::R32F    },
		{ PVR3_RG8,              PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RG8     },
		{ PVR3_RG16,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RG16    },
		{ PVR3_RG16,             PVR3_CHANNEL_TYPE_FLOAT, TextureFormat::RG16F   },
		{ PVR3_RG32,             PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RG16    },
		{ PVR3_RG32,             PVR3_CHANNEL_TYPE_FLOAT, TextureFormat::RG32F   },
		{ PVR3_BGRA8,            PVR3_CHANNEL_TYPE_ANY,   TextureFormat::BGRA8   },
		{ PVR3_RGBA16,           PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RGBA16  },
		{ PVR3_RGBA16,           PVR3_CHANNEL_TYPE_FLOAT, TextureFormat::RGBA16F },
		{ PVR3_RGBA32,           PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RGBA32U },
		{ PVR3_RGBA32,           PVR3_CHANNEL_TYPE_FLOAT, TextureFormat::RGBA32F },
		{ PVR3_RGB565,           PVR3_CHANNEL_TYPE_ANY,   TextureFormat::R5G6B5  },
		{ PVR3_RGBA4,            PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RGBA4   },
		{ PVR3_RGBA51,           PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RGB5A1  },
		{ PVR3_RGB10A2,          PVR3_CHANNEL_TYPE_ANY,   TextureFormat::RGB10A2 },
	};

	bool imageParsePvr3(ImageContainer& _imageContainer, bx::ReaderSeekerI* _reader)
	{
		uint32_t flags;
		bx::read(_reader, flags);

		uint64_t pixelFormat;
		bx::read(_reader, pixelFormat);

		uint32_t colorSpace;
		bx::read(_reader, colorSpace); // 0 - linearRGB, 1 - sRGB

		uint32_t channelType;
		bx::read(_reader, channelType);

		uint32_t height;
		bx::read(_reader, height);

		uint32_t width;
		bx::read(_reader, width);

		uint32_t depth;
		bx::read(_reader, depth);

		uint32_t numSurfaces;
		bx::read(_reader, numSurfaces);

		uint32_t numFaces;
		bx::read(_reader, numFaces);

		uint32_t numMips;
		bx::read(_reader, numMips);

		uint32_t metaDataSize;
		bx::read(_reader, metaDataSize);

		// skip meta garbage...
		int64_t offset = bx::skip(_reader, metaDataSize);

		TextureFormat::Enum format = TextureFormat::Unknown;
		bool hasAlpha = false;

		for (uint32_t ii = 0; ii < BX_COUNTOF(s_translatePvr3Format); ++ii)
		{
			if (s_translatePvr3Format[ii].m_format == pixelFormat
			&&  channelType == (s_translatePvr3Format[ii].m_channelTypeMask & channelType) )
			{
				format = s_translatePvr3Format[ii].m_textureFormat;
				break;
			}
		}

		_imageContainer.m_data = NULL;
		_imageContainer.m_size = 0;
		_imageContainer.m_offset = (uint32_t)offset;
		_imageContainer.m_width  = width;
		_imageContainer.m_height = height;
		_imageContainer.m_depth  = depth;
		_imageContainer.m_format = uint8_t(format);
		_imageContainer.m_numMips  = uint8_t(numMips);
		_imageContainer.m_hasAlpha = hasAlpha;
		_imageContainer.m_cubeMap  = numFaces > 1;
		_imageContainer.m_ktx = false;
		_imageContainer.m_srgb = colorSpace > 0;

		return TextureFormat::Unknown != format;
	}

	bool imageParse(ImageContainer& _imageContainer, bx::ReaderSeekerI* _reader)
	{
		uint32_t magic;
		bx::read(_reader, magic);

		if (DDS_MAGIC == magic)
		{
			return imageParseDds(_imageContainer, _reader);
		}
		else if (KTX_MAGIC == magic)
		{
			return imageParseKtx(_imageContainer, _reader);
		}
		else if (PVR3_MAGIC == magic)
		{
			return imageParsePvr3(_imageContainer, _reader);
		}
		else if (BGFX_CHUNK_MAGIC_TEX == magic)
		{
			TextureCreate tc;
			bx::read(_reader, tc);

			_imageContainer.m_format = tc.m_format;
			_imageContainer.m_offset = UINT32_MAX;
			if (NULL == tc.m_mem)
			{
				_imageContainer.m_data = NULL;
				_imageContainer.m_size = 0;
			}
			else
			{
				_imageContainer.m_data = tc.m_mem->data;
				_imageContainer.m_size = tc.m_mem->size;
			}
			_imageContainer.m_width = tc.m_width;
			_imageContainer.m_height = tc.m_height;
			_imageContainer.m_depth = tc.m_depth;
			_imageContainer.m_numMips = tc.m_numMips;
			_imageContainer.m_hasAlpha = false;
			_imageContainer.m_cubeMap = tc.m_cubeMap;
			_imageContainer.m_ktx = false;
			_imageContainer.m_srgb = false;

			return true;
		}

		return false;
	}

	bool imageParse(ImageContainer& _imageContainer, const void* _data, uint32_t _size)
	{
		bx::MemoryReader reader(_data, _size);
		return imageParse(_imageContainer, &reader);
	}

	void imageDecodeToBgra8(uint8_t* _dst, const uint8_t* _src, uint32_t _width, uint32_t _height, uint32_t _pitch, uint8_t _type)
	{
		const uint8_t* src = _src;

		uint32_t width  = _width/4;
		uint32_t height = _height/4;

		uint8_t temp[16*4];

		switch (_type)
		{
		case TextureFormat::BC1:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockDxt1(temp, src);
					src += 8;

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::BC2:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockDxt23A(temp+3, src);
					src += 8;
					decodeBlockDxt(temp, src);
					src += 8;

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::BC3:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockDxt45A(temp+3, src);
					src += 8;
					decodeBlockDxt(temp, src);
					src += 8;

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::BC4:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockDxt45A(temp, src);
					src += 8;

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::BC5:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockDxt45A(temp+1, src);
					src += 8;
					decodeBlockDxt45A(temp+2, src);
					src += 8;

					for (uint32_t ii = 0; ii < 16; ++ii)
					{
						float nx = temp[ii*4+2]*2.0f/255.0f - 1.0f;
						float ny = temp[ii*4+1]*2.0f/255.0f - 1.0f;
						float nz = sqrtf(1.0f - nx*nx - ny*ny);
						temp[ii*4+0] = uint8_t( (nz + 1.0f)*255.0f/2.0f);
						temp[ii*4+3] = 0;
					}

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::ETC1:
		case TextureFormat::ETC2:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockEtc12(temp, src);
					src += 8;

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::ETC2A:
			BX_WARN(false, "ETC2A decoder is not implemented.");
			imageCheckerboard(_width, _height, 16, UINT32_C(0xff000000), UINT32_C(0xff00ff00), _dst);
			break;

		case TextureFormat::ETC2A1:
			BX_WARN(false, "ETC2A1 decoder is not implemented.");
			imageCheckerboard(_width, _height, 16, UINT32_C(0xff000000), UINT32_C(0xffff0000), _dst);
			break;

		case TextureFormat::PTC12:
			BX_WARN(false, "PTC12 decoder is not implemented.");
			imageCheckerboard(_width, _height, 16, UINT32_C(0xff000000), UINT32_C(0xffff00ff), _dst);
			break;

		case TextureFormat::PTC12A:
			BX_WARN(false, "PTC12A decoder is not implemented.");
			imageCheckerboard(_width, _height, 16, UINT32_C(0xff000000), UINT32_C(0xffffff00), _dst);
			break;

		case TextureFormat::PTC14:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockPtc14(temp, src, xx, yy, width, height);

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::PTC14A:
			for (uint32_t yy = 0; yy < height; ++yy)
			{
				for (uint32_t xx = 0; xx < width; ++xx)
				{
					decodeBlockPtc14A(temp, src, xx, yy, width, height);

					uint8_t* dst = &_dst[(yy*_pitch+xx*4)*4];
					memcpy(&dst[0*_pitch], &temp[ 0], 16);
					memcpy(&dst[1*_pitch], &temp[16], 16);
					memcpy(&dst[2*_pitch], &temp[32], 16);
					memcpy(&dst[3*_pitch], &temp[48], 16);
				}
			}
			break;

		case TextureFormat::PTC22:
			BX_WARN(false, "PTC22 decoder is not implemented.");
			imageCheckerboard(_width, _height, 16, UINT32_C(0xff00ff00), UINT32_C(0xff0000ff), _dst);
			break;

		case TextureFormat::PTC24:
			BX_WARN(false, "PTC24 decoder is not implemented.");
			imageCheckerboard(_width, _height, 16, UINT32_C(0xff000000), UINT32_C(0xffffffff), _dst);
			break;

		case TextureFormat::RGBA8:
			imageSwizzleBgra8(_width, _height, _pitch, _src, _dst);
			break;

		case TextureFormat::BGRA8:
			memcpy(_dst, _src, _pitch*_height);
			break;

		default:
			// Decompression not implemented... Make ugly red-yellow checkerboard texture.
			imageCheckerboard(_width, _height, 16, UINT32_C(0xffff0000), UINT32_C(0xffffff00), _dst);
			break;
		}
	}

	void imageDecodeToRgba8(uint8_t* _dst, const uint8_t* _src, uint32_t _width, uint32_t _height, uint32_t _pitch, uint8_t _type)
	{
		switch (_type)
		{
		case TextureFormat::RGBA8:
			memcpy(_dst, _src, _pitch*_height);
			break;

		case TextureFormat::BGRA8:
			imageSwizzleBgra8(_width, _height, _pitch, _src, _dst);
			break;

		default:
			imageDecodeToBgra8(_dst, _src, _width, _height, _pitch, _type);
			imageSwizzleBgra8(_width, _height, _pitch, _dst, _dst);
			break;
		}
	}

	bool imageGetRawData(const ImageContainer& _imageContainer, uint8_t _side, uint8_t _lod, const void* _data, uint32_t _size, ImageMip& _mip)
	{
		uint32_t offset = _imageContainer.m_offset;
		TextureFormat::Enum type = TextureFormat::Enum(_imageContainer.m_format);
		bool hasAlpha = _imageContainer.m_hasAlpha;

		const ImageBlockInfo& blockInfo = s_imageBlockInfo[type];
		const uint8_t  bpp         = blockInfo.bitsPerPixel;
		const uint32_t blockSize   = blockInfo.blockSize;
		const uint32_t blockWidth  = blockInfo.blockWidth;
		const uint32_t blockHeight = blockInfo.blockHeight;
		const uint32_t minBlockX   = blockInfo.minBlockX;
		const uint32_t minBlockY   = blockInfo.minBlockY;

		if (UINT32_MAX == _imageContainer.m_offset)
		{
			if (NULL == _imageContainer.m_data)
			{
				return false;
			}

			offset = 0;
			_data = _imageContainer.m_data;
			_size = _imageContainer.m_size;
		}

		for (uint8_t side = 0, numSides = _imageContainer.m_cubeMap ? 6 : 1; side < numSides; ++side)
		{
			uint32_t width  = _imageContainer.m_width;
			uint32_t height = _imageContainer.m_height;
			uint32_t depth  = _imageContainer.m_depth;

			for (uint8_t lod = 0, num = _imageContainer.m_numMips; lod < num; ++lod)
			{
				// skip imageSize in KTX format.
				offset += _imageContainer.m_ktx ? sizeof(uint32_t) : 0;

				width  = bx::uint32_max(blockWidth  * minBlockX, ( (width  + blockWidth  - 1) / blockWidth )*blockWidth);
				height = bx::uint32_max(blockHeight * minBlockY, ( (height + blockHeight - 1) / blockHeight)*blockHeight);
				depth  = bx::uint32_max(1, depth);

				uint32_t size = width*height*depth*bpp/8;

				if (side == _side
				&&  lod == _lod)
				{
					_mip.m_width     = width;
					_mip.m_height    = height;
					_mip.m_blockSize = blockSize;
					_mip.m_size = size;
					_mip.m_data = (const uint8_t*)_data + offset;
					_mip.m_bpp  = bpp;
					_mip.m_format   = uint8_t(type);
					_mip.m_hasAlpha = hasAlpha;
					return true;
				}

				offset += size;

				BX_CHECK(offset <= _size, "Reading past size of data buffer! (offset %d, size %d)", offset, _size);
				BX_UNUSED(_size);

				width  >>= 1;
				height >>= 1;
				depth  >>= 1;
			}
		}

		return false;
	}

} // namespace bgfx
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "ovr.h"

#if BGFX_CONFIG_USE_OVR

namespace bgfx
{
#if OVR_VERSION <= OVR_VERSION_050
#	define OVR_EYE_BUFFER 100
#else
#	define OVR_EYE_BUFFER 8
#endif // OVR_VERSION...

	OVR::OVR()
		: m_hmd(NULL)
		, m_isenabled(false)
		, m_debug(false)
	{
	}

	OVR::~OVR()
	{
		BX_CHECK(NULL == m_hmd, "OVR not shutdown properly.");
	}

	void OVR::init()
	{
		bool initialized = !!ovr_Initialize();
		BX_WARN(initialized, "Unable to create OVR device.");
		if (!initialized)
		{
			return;
		}

		m_hmd = ovrHmd_Create(0);
		if (NULL == m_hmd)
		{
			m_hmd = ovrHmd_CreateDebug(ovrHmd_DK2);
			BX_WARN(NULL != m_hmd, "Unable to create OVR device.");
			if (NULL == m_hmd)
			{
				return;
			}
		}

		BX_TRACE("HMD: %s, %s, firmware: %d.%d"
			, m_hmd->ProductName
			, m_hmd->Manufacturer
			, m_hmd->FirmwareMajor
			, m_hmd->FirmwareMinor
			);

		ovrSizei sizeL = ovrHmd_GetFovTextureSize(m_hmd, ovrEye_Left,  m_hmd->DefaultEyeFov[0], 1.0f);
		ovrSizei sizeR = ovrHmd_GetFovTextureSize(m_hmd, ovrEye_Right, m_hmd->DefaultEyeFov[1], 1.0f);
		m_rtSize.w = sizeL.w + sizeR.w + OVR_EYE_BUFFER;
		m_rtSize.h = bx::uint32_max(sizeL.h, sizeR.h);
		m_warning = true;
	}

	void OVR::shutdown()
	{
		BX_CHECK(!m_isenabled, "HMD not disabled.");
		ovrHmd_Destroy(m_hmd);
		m_hmd = NULL;
		ovr_Shutdown();
	}

	void OVR::getViewport(uint8_t _eye, Rect* _viewport)
	{
		_viewport->m_x      = _eye * (m_rtSize.w + OVR_EYE_BUFFER + 1)/2;
		_viewport->m_y      = 0;
		_viewport->m_width  = (m_rtSize.w - OVR_EYE_BUFFER)/2;
		_viewport->m_height = m_rtSize.h;
	}

	bool OVR::postReset(void* _nwh, ovrRenderAPIConfig* _config, bool _debug)
	{
		if (_debug)
		{
			switch (_config->Header.API)
			{
#if BGFX_CONFIG_RENDERER_DIRECT3D11
			case ovrRenderAPI_D3D11:
				{
					ovrD3D11ConfigData* data = (ovrD3D11ConfigData*)_config;
#	if OVR_VERSION > OVR_VERSION_043
					m_rtSize = data->Header.BackBufferSize;
#	else
					m_rtSize = data->Header.RTSize;
#	endif // OVR_VERSION > OVR_VERSION_043
				}
				break;
#endif // BGFX_CONFIG_RENDERER_DIRECT3D11

#if BGFX_CONFIG_RENDERER_OPENGL
			case ovrRenderAPI_OpenGL:
				{
					ovrGLConfigData* data = (ovrGLConfigData*)_config;
#	if OVR_VERSION > OVR_VERSION_043
					m_rtSize = data->Header.BackBufferSize;
#	else
					m_rtSize = data->Header.RTSize;
#	endif // OVR_VERSION > OVR_VERSION_043
				}
				break;
#endif // BGFX_CONFIG_RENDERER_OPENGL

			case ovrRenderAPI_None:
			default:
				BX_CHECK(false, "You should not be here!");
				break;
			}

			m_debug = true;
			return false;
		}

		if (NULL == m_hmd)
		{
			return false;
		}

		m_isenabled = true;

		ovrBool result;
		result = ovrHmd_AttachToWindow(m_hmd, _nwh, NULL, NULL);
		if (!result) { goto ovrError; }

		ovrFovPort eyeFov[2] = { m_hmd->DefaultEyeFov[0], m_hmd->DefaultEyeFov[1] };
		result = ovrHmd_ConfigureRendering(m_hmd
			, _config
			, 0
#if OVR_VERSION < OVR_VERSION_050
			| ovrDistortionCap_Chromatic // permanently enabled >= v5.0
#endif
			| ovrDistortionCap_Vignette
			| ovrDistortionCap_TimeWarp
			| ovrDistortionCap_Overdrive
			| ovrDistortionCap_NoRestore
			| ovrDistortionCap_HqDistortion
			, eyeFov
			, m_erd
			);
		if (!result) { goto ovrError; }

		ovrHmd_SetEnabledCaps(m_hmd
			, 0
			| ovrHmdCap_LowPersistence
			| ovrHmdCap_DynamicPrediction
			);

		result = ovrHmd_ConfigureTracking(m_hmd
			, 0
			| ovrTrackingCap_Orientation
			| ovrTrackingCap_MagYawCorrection
			| ovrTrackingCap_Position
			, 0
			);

		if (!result)
		{
ovrError:
			BX_TRACE("Failed to initialize OVR.");
			m_isenabled = false;
			return false;
		}

		m_warning = true;
		return true;
	}

	void OVR::postReset(const ovrTexture& _texture)
	{
		if (NULL != m_hmd)
		{
			m_texture[0] = _texture;
			m_texture[1] = _texture;

			ovrRecti rect;
			rect.Pos.x  = 0;
			rect.Pos.y  = 0;
			rect.Size.w = (m_rtSize.w - OVR_EYE_BUFFER)/2;
			rect.Size.h = m_rtSize.h;

			m_texture[0].Header.RenderViewport = rect;

			rect.Pos.x += rect.Size.w + OVR_EYE_BUFFER;
			m_texture[1].Header.RenderViewport = rect;

			m_timing = ovrHmd_BeginFrame(m_hmd, 0);
#if OVR_VERSION > OVR_VERSION_042
			m_pose[0] = ovrHmd_GetHmdPosePerEye(m_hmd, ovrEye_Left);
			m_pose[1] = ovrHmd_GetHmdPosePerEye(m_hmd, ovrEye_Right);
#else
			m_pose[0] = ovrHmd_GetEyePose(m_hmd, ovrEye_Left);
			m_pose[1] = ovrHmd_GetEyePose(m_hmd, ovrEye_Right);
#endif // OVR_VERSION > OVR_VERSION_042
		}
	}

	void OVR::preReset()
	{
		if (m_isenabled)
		{
			ovrHmd_EndFrame(m_hmd, m_pose, m_texture);
			ovrHmd_AttachToWindow(m_hmd, NULL, NULL, NULL);
			ovrHmd_ConfigureRendering(m_hmd, NULL, 0, NULL, NULL);
			m_isenabled = false;
		}

		m_debug = false;
	}

	bool OVR::swap(HMD& _hmd)
	{
		_hmd.flags = BGFX_HMD_NONE;

		if (NULL != m_hmd)
		{
			_hmd.flags |= BGFX_HMD_DEVICE_RESOLUTION;
			_hmd.deviceWidth  = m_hmd->Resolution.w;
			_hmd.deviceHeight = m_hmd->Resolution.h;
		}

		if (!m_isenabled)
		{
			return false;
		}

		_hmd.flags |= BGFX_HMD_RENDERING;
		ovrHmd_EndFrame(m_hmd, m_pose, m_texture);

		if (m_warning)
		{
			m_warning = !ovrHmd_DismissHSWDisplay(m_hmd);
		}

		m_timing = ovrHmd_BeginFrame(m_hmd, 0);

#if OVR_VERSION > OVR_VERSION_042
		m_pose[0] = ovrHmd_GetHmdPosePerEye(m_hmd, ovrEye_Left);
		m_pose[1] = ovrHmd_GetHmdPosePerEye(m_hmd, ovrEye_Right);
#else
		m_pose[0] = ovrHmd_GetEyePose(m_hmd, ovrEye_Left);
		m_pose[1] = ovrHmd_GetEyePose(m_hmd, ovrEye_Right);
#endif // OVR_VERSION > OVR_VERSION_042

		getEyePose(_hmd);

		return true;
	}

	void OVR::recenter()
	{
		if (NULL != m_hmd)
		{
			ovrHmd_RecenterPose(m_hmd);
		}
	}

	void OVR::getEyePose(HMD& _hmd)
	{
		if (NULL != m_hmd)
		{
			for (int ii = 0; ii < 2; ++ii)
			{
				const ovrPosef& pose = m_pose[ii];
				HMD::Eye& eye = _hmd.eye[ii];
				eye.rotation[0] = pose.Orientation.x;
				eye.rotation[1] = pose.Orientation.y;
				eye.rotation[2] = pose.Orientation.z;
				eye.rotation[3] = pose.Orientation.w;
				eye.translation[0] = pose.Position.x;
				eye.translation[1] = pose.Position.y;
				eye.translation[2] = pose.Position.z;

				const ovrEyeRenderDesc& erd = m_erd[ii];
				eye.fov[0] = erd.Fov.UpTan;
				eye.fov[1] = erd.Fov.DownTan;
				eye.fov[2] = erd.Fov.LeftTan;
				eye.fov[3] = erd.Fov.RightTan;
#if OVR_VERSION > OVR_VERSION_042
				eye.viewOffset[0] = erd.HmdToEyeViewOffset.x;
				eye.viewOffset[1] = erd.HmdToEyeViewOffset.y;
				eye.viewOffset[2] = erd.HmdToEyeViewOffset.z;
#else
				eye.viewOffset[0] = erd.ViewAdjust.x;
				eye.viewOffset[1] = erd.ViewAdjust.y;
				eye.viewOffset[2] = erd.ViewAdjust.z;
#endif // OVR_VERSION > OVR_VERSION_042
				eye.pixelsPerTanAngle[0] = erd.PixelsPerTanAngleAtCenter.x;
				eye.pixelsPerTanAngle[1] = erd.PixelsPerTanAngleAtCenter.y;
			}
		}
		else
		{
			for (int ii = 0; ii < 2; ++ii)
			{
				_hmd.eye[ii].rotation[0] = 0.0f;
				_hmd.eye[ii].rotation[1] = 0.0f;
				_hmd.eye[ii].rotation[2] = 0.0f;
				_hmd.eye[ii].rotation[3] = 1.0f;
				_hmd.eye[ii].translation[0] = 0.0f;
				_hmd.eye[ii].translation[1] = 0.0f;
				_hmd.eye[ii].translation[2] = 0.0f;
				_hmd.eye[ii].fov[0] = 1.32928634f;
				_hmd.eye[ii].fov[1] = 1.32928634f;
				_hmd.eye[ii].fov[2] = 0 == ii ? 1.05865765f : 1.09236801f;
				_hmd.eye[ii].fov[3] = 0 == ii ? 1.09236801f : 1.05865765f;
				_hmd.eye[ii].viewOffset[0] = 0 == ii ? 0.0355070010f  : -0.0375000015f;
				_hmd.eye[ii].viewOffset[1] = 0.0f;
				_hmd.eye[ii].viewOffset[2] = 0 == ii ? 0.00150949787f : -0.00150949787f;
				_hmd.eye[ii].pixelsPerTanAngle[0] = 1;
				_hmd.eye[ii].pixelsPerTanAngle[1] = 1;
			}
		}

		_hmd.width  = uint16_t(m_rtSize.w);
		_hmd.height = uint16_t(m_rtSize.h);
	}

} // namespace bgfx

#endif // BGFX_CONFIG_USE_OVR
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BGFX_CONFIG_DEBUG_PIX && (BX_PLATFORM_WINDOWS || BX_PLATFORM_LINUX)
#	if BX_PLATFORM_WINDOWS
#		include <psapi.h>
#	endif // BX_PLATFORM_WINDOWS
#	include <renderdoc/renderdoc_app.h>

namespace bgfx
{
	bool findModule(const char* _name)
	{
#if BX_PLATFORM_WINDOWS
		HANDLE process = GetCurrentProcess();
		DWORD size;
		BOOL result = EnumProcessModules(process
						, NULL
						, 0
						, &size
						);
		if (0 != result)
		{
			HMODULE* modules = (HMODULE*)alloca(size);
			result = EnumProcessModules(process
				, modules
				, size
				, &size
				);

			if (0 != result)
			{
				char moduleName[MAX_PATH];
				for (uint32_t ii = 0, num = uint32_t(size/sizeof(HMODULE) ); ii < num; ++ii)
				{
					result = GetModuleBaseNameA(process
								, modules[ii]
								, moduleName
								, BX_COUNTOF(moduleName)
								);
					if (0 != result
					&&  0 == bx::stricmp(_name, moduleName) )
					{
						return true;
					}
				}
			}
		}
#endif // BX_PLATFORM_WINDOWS
		BX_UNUSED(_name);
		return false;
	}

#define RENDERDOC_IMPORT \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_Shutdown); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_SetLogFile); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_GetCapture); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_SetCaptureOptions); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_SetActiveWindow); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_TriggerCapture); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_StartFrameCapture); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_EndFrameCapture); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_GetOverlayBits); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_MaskOverlayBits); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_SetFocusToggleKeys); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_SetCaptureKeys); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_InitRemoteAccess); \
			RENDERDOC_IMPORT_FUNC(RENDERDOC_UnloadCrashHandler);

#define RENDERDOC_IMPORT_FUNC(_func) p##_func _func
	RENDERDOC_IMPORT
#undef RENDERDOC_IMPORT_FUNC

	pRENDERDOC_GetAPIVersion RENDERDOC_GetAPIVersion;

	void* loadRenderDoc()
	{
		// Skip loading RenderDoc when IntelGPA is present to avoid RenderDoc crash.
		if (findModule(BX_ARCH_32BIT ? "shimloader32.dll" : "shimloader64.dll") )
		{
			return NULL;
		}

		void* renderdocdll = bx::dlopen("renderdoc.dll");

		if (NULL != renderdocdll)
		{
			RENDERDOC_GetAPIVersion = (pRENDERDOC_GetAPIVersion)bx::dlsym(renderdocdll, "RENDERDOC_GetAPIVersion");
			if (NULL != RENDERDOC_GetAPIVersion
			&&  RENDERDOC_API_VERSION == RENDERDOC_GetAPIVersion() )
			{
#define RENDERDOC_IMPORT_FUNC(_func) \
			_func = (p##_func)bx::dlsym(renderdocdll, #_func); \
			BX_TRACE("%p " #_func, _func);
RENDERDOC_IMPORT
#undef RENDERDOC_IMPORT_FUNC

				RENDERDOC_SetLogFile("temp/bgfx");

				RENDERDOC_SetFocusToggleKeys(NULL, 0);

				KeyButton captureKey = eKey_F11;
				RENDERDOC_SetCaptureKeys(&captureKey, 1);

				CaptureOptions opt;
				memset(&opt, 0, sizeof(opt) );
				opt.AllowVSync      = 1;
				opt.SaveAllInitials = 1;
				RENDERDOC_SetCaptureOptions(&opt);

				uint32_t ident = 0;
				RENDERDOC_InitRemoteAccess(&ident);

				RENDERDOC_MaskOverlayBits(eOverlay_None, eOverlay_None);
			}
			else
			{
				bx::dlclose(renderdocdll);
				renderdocdll = NULL;
			}
		}

		return renderdocdll;
	}

	void unloadRenderDoc(void* _renderdocdll)
	{
		if (NULL != _renderdocdll)
		{
			RENDERDOC_Shutdown();
			bx::dlclose(_renderdocdll);
		}
	}

} // namespace bgfx

#else

namespace bgfx
{

	void* loadRenderDoc()
	{
		return NULL;
	}

	void unloadRenderDoc(void*)
	{
	}

} // namespace bgfx

#endif // BGFX_CONFIG_DEBUG_PIX && (BX_PLATFORM_WINDOWS || BX_PLATFORM_LINUX)
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_DIRECT3D11
#	include "renderer_d3d11.h"

namespace bgfx { namespace d3d11
{
	static wchar_t s_viewNameW[BGFX_CONFIG_MAX_VIEWS][BGFX_CONFIG_MAX_VIEW_NAME];

	struct PrimInfo
	{
		D3D11_PRIMITIVE_TOPOLOGY m_type;
		uint32_t m_min;
		uint32_t m_div;
		uint32_t m_sub;
	};

	static const PrimInfo s_primInfo[] =
	{
		{ D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,  3, 3, 0 },
		{ D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 3, 1, 2 },
		{ D3D11_PRIMITIVE_TOPOLOGY_LINELIST,      2, 2, 0 },
		{ D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,     2, 1, 1 },
		{ D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,     1, 1, 0 },
		{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,     0, 0, 0 },
	};

	static const char* s_primName[] =
	{
		"TriList",
		"TriStrip",
		"Line",
		"LineStrip",
		"Point",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_primInfo) == BX_COUNTOF(s_primName)+1);

	union Zero
	{
		Zero()
		{
			memset(this, 0, sizeof(Zero) );
		}

		ID3D11Buffer*              m_buffer[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
		ID3D11UnorderedAccessView* m_uav[D3D11_PS_CS_UAV_REGISTER_COUNT];
		ID3D11ShaderResourceView*  m_srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
		ID3D11SamplerState*        m_sampler[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
		uint32_t                   m_zero[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	};

	BX_PRAGMA_DIAGNOSTIC_PUSH();
	BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4268) // warning C4268: '' : 'const' static/global data initialized with compiler generated default constructor fills the object with zeros
	static const Zero s_zero;
	BX_PRAGMA_DIAGNOSTIC_POP();

	static const uint32_t s_checkMsaa[] =
	{
		0,
		2,
		4,
		8,
		16,
	};

	static DXGI_SAMPLE_DESC s_msaa[] =
	{
		{  1, 0 },
		{  2, 0 },
		{  4, 0 },
		{  8, 0 },
		{ 16, 0 },
	};

	static const D3D11_BLEND s_blendFactor[][2] =
	{
		{ (D3D11_BLEND)0,               (D3D11_BLEND)0               }, // ignored
		{ D3D11_BLEND_ZERO,             D3D11_BLEND_ZERO             }, // ZERO
		{ D3D11_BLEND_ONE,              D3D11_BLEND_ONE              },	// ONE
		{ D3D11_BLEND_SRC_COLOR,        D3D11_BLEND_SRC_ALPHA        },	// SRC_COLOR
		{ D3D11_BLEND_INV_SRC_COLOR,    D3D11_BLEND_INV_SRC_ALPHA    },	// INV_SRC_COLOR
		{ D3D11_BLEND_SRC_ALPHA,        D3D11_BLEND_SRC_ALPHA        },	// SRC_ALPHA
		{ D3D11_BLEND_INV_SRC_ALPHA,    D3D11_BLEND_INV_SRC_ALPHA    },	// INV_SRC_ALPHA
		{ D3D11_BLEND_DEST_ALPHA,       D3D11_BLEND_DEST_ALPHA       },	// DST_ALPHA
		{ D3D11_BLEND_INV_DEST_ALPHA,   D3D11_BLEND_INV_DEST_ALPHA   },	// INV_DST_ALPHA
		{ D3D11_BLEND_DEST_COLOR,       D3D11_BLEND_DEST_ALPHA       },	// DST_COLOR
		{ D3D11_BLEND_INV_DEST_COLOR,   D3D11_BLEND_INV_DEST_ALPHA   },	// INV_DST_COLOR
		{ D3D11_BLEND_SRC_ALPHA_SAT,    D3D11_BLEND_ONE              },	// SRC_ALPHA_SAT
		{ D3D11_BLEND_BLEND_FACTOR,     D3D11_BLEND_BLEND_FACTOR     },	// FACTOR
		{ D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR },	// INV_FACTOR
	};

	static const D3D11_BLEND_OP s_blendEquation[] =
	{
		D3D11_BLEND_OP_ADD,
		D3D11_BLEND_OP_SUBTRACT,
		D3D11_BLEND_OP_REV_SUBTRACT,
		D3D11_BLEND_OP_MIN,
		D3D11_BLEND_OP_MAX,
	};

	static const D3D11_COMPARISON_FUNC s_cmpFunc[] =
	{
		D3D11_COMPARISON_FUNC(0), // ignored
		D3D11_COMPARISON_LESS,
		D3D11_COMPARISON_LESS_EQUAL,
		D3D11_COMPARISON_EQUAL,
		D3D11_COMPARISON_GREATER_EQUAL,
		D3D11_COMPARISON_GREATER,
		D3D11_COMPARISON_NOT_EQUAL,
		D3D11_COMPARISON_NEVER,
		D3D11_COMPARISON_ALWAYS,
	};

	static const D3D11_STENCIL_OP s_stencilOp[] =
	{
		D3D11_STENCIL_OP_ZERO,
		D3D11_STENCIL_OP_KEEP,
		D3D11_STENCIL_OP_REPLACE,
		D3D11_STENCIL_OP_INCR,
		D3D11_STENCIL_OP_INCR_SAT,
		D3D11_STENCIL_OP_DECR,
		D3D11_STENCIL_OP_DECR_SAT,
		D3D11_STENCIL_OP_INVERT,
	};

	static const D3D11_CULL_MODE s_cullMode[] =
	{
		D3D11_CULL_NONE,
		D3D11_CULL_FRONT,
		D3D11_CULL_BACK,
	};

	static const D3D11_TEXTURE_ADDRESS_MODE s_textureAddress[] =
	{
		D3D11_TEXTURE_ADDRESS_WRAP,
		D3D11_TEXTURE_ADDRESS_MIRROR,
		D3D11_TEXTURE_ADDRESS_CLAMP,
	};

	/*
	 * D3D11_FILTER_MIN_MAG_MIP_POINT               = 0x00,
	 * D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR        = 0x01,
	 * D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT  = 0x04,
	 * D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR        = 0x05,
	 * D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT        = 0x10,
	 * D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
	 * D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT        = 0x14,
	 * D3D11_FILTER_MIN_MAG_MIP_LINEAR              = 0x15,
	 * D3D11_FILTER_ANISOTROPIC                     = 0x55,
	 *
	 * D3D11_COMPARISON_FILTERING_BIT               = 0x80,
	 * D3D11_ANISOTROPIC_FILTERING_BIT              = 0x40,
	 *
	 * According to D3D11_FILTER enum bits for mip, mag and mip are:
	 * 0x10 // MIN_LINEAR
	 * 0x04 // MAG_LINEAR
	 * 0x01 // MIP_LINEAR
	 */

	static const uint8_t s_textureFilter[3][3] =
	{
		{
			0x10, // min linear
			0x00, // min point
			0x55, // anisotropic
		},
		{
			0x04, // mag linear
			0x00, // mag point
			0x55, // anisotropic
		},
		{
			0x01, // mip linear
			0x00, // mip point
			0x55, // anisotropic
		},
	};

	struct TextureFormatInfo
	{
		DXGI_FORMAT m_fmt;
		DXGI_FORMAT m_fmtSrv;
		DXGI_FORMAT m_fmtDsv;
		DXGI_FORMAT m_fmtSrgb;
	};

	static const TextureFormatInfo s_textureFormat[] =
	{
		{ DXGI_FORMAT_BC1_UNORM,          DXGI_FORMAT_BC1_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC1_UNORM_SRGB      }, // BC1
		{ DXGI_FORMAT_BC2_UNORM,          DXGI_FORMAT_BC2_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC2_UNORM_SRGB      }, // BC2
		{ DXGI_FORMAT_BC3_UNORM,          DXGI_FORMAT_BC3_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC3_UNORM_SRGB      }, // BC3
		{ DXGI_FORMAT_BC4_UNORM,          DXGI_FORMAT_BC4_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // BC4
		{ DXGI_FORMAT_BC5_UNORM,          DXGI_FORMAT_BC5_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // BC5
		{ DXGI_FORMAT_BC6H_SF16,          DXGI_FORMAT_BC6H_SF16,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // BC6H
		{ DXGI_FORMAT_BC7_UNORM,          DXGI_FORMAT_BC7_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC7_UNORM_SRGB      }, // BC7
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC1
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC2
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC2A
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC2A1
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC12
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC14
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC12A
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC14A
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC22
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC24
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // Unknown
		{ DXGI_FORMAT_R1_UNORM,           DXGI_FORMAT_R1_UNORM,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R1
		{ DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8_UNORM,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8
		{ DXGI_FORMAT_R8_SINT,            DXGI_FORMAT_R8_SINT,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8I
		{ DXGI_FORMAT_R8_UINT,            DXGI_FORMAT_R8_UINT,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8U
		{ DXGI_FORMAT_R8_SNORM,           DXGI_FORMAT_R8_SNORM,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8S
		{ DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_R16_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16
		{ DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SINT,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16I
		{ DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_R16_UINT,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16U
		{ DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16F
		{ DXGI_FORMAT_R16_SNORM,          DXGI_FORMAT_R16_SNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16S
		{ DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_UINT,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R32U
		{ DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R32F
		{ DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R8G8_UNORM,            DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8
		{ DXGI_FORMAT_R8G8_SINT,          DXGI_FORMAT_R8G8_SINT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8I
		{ DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_R8G8_UINT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8U
		{ DXGI_FORMAT_R8G8_SNORM,         DXGI_FORMAT_R8G8_SNORM,            DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8S
		{ DXGI_FORMAT_R16G16_UNORM,       DXGI_FORMAT_R16G16_UNORM,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16
		{ DXGI_FORMAT_R16G16_SINT,        DXGI_FORMAT_R16G16_SINT,           DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16I
		{ DXGI_FORMAT_R16G16_UINT,        DXGI_FORMAT_R16G16_UINT,           DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16U
		{ DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16F
		{ DXGI_FORMAT_R16G16_SNORM,       DXGI_FORMAT_R16G16_SNORM,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16S
		{ DXGI_FORMAT_R32G32_UINT,        DXGI_FORMAT_R32G32_UINT,           DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG32U
		{ DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG32F
		{ DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_B8G8R8A8_UNORM_SRGB }, // BGRA8
		{ DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }, // RGBA8
		{ DXGI_FORMAT_R8G8B8A8_SINT,      DXGI_FORMAT_R8G8B8A8_SINT,         DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }, // RGBA8I
		{ DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R8G8B8A8_UINT,         DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }, // RGBA8U
		{ DXGI_FORMAT_R8G8B8A8_SNORM,     DXGI_FORMAT_R8G8B8A8_SNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA8S
		{ DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16
		{ DXGI_FORMAT_R16G16B16A16_SINT,  DXGI_FORMAT_R16G16B16A16_SINT,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16I
		{ DXGI_FORMAT_R16G16B16A16_UINT,  DXGI_FORMAT_R16G16B16A16_UINT,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16U
		{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16F
		{ DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16S
		{ DXGI_FORMAT_R32G32B32A32_UINT,  DXGI_FORMAT_R32G32B32A32_UINT,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA32U
		{ DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA32F
		{ DXGI_FORMAT_B5G6R5_UNORM,       DXGI_FORMAT_B5G6R5_UNORM,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R5G6B5
		{ DXGI_FORMAT_B4G4R4A4_UNORM,     DXGI_FORMAT_B4G4R4A4_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA4
		{ DXGI_FORMAT_B5G5R5A1_UNORM,     DXGI_FORMAT_B5G5R5A1_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGB5A1
		{ DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R10G10B10A2_UNORM,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGB10A2
		{ DXGI_FORMAT_R11G11B10_FLOAT,    DXGI_FORMAT_R11G11B10_FLOAT,       DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R11G11B10F
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // UnknownDepth
		{ DXGI_FORMAT_R16_TYPELESS,       DXGI_FORMAT_R16_UNORM,             DXGI_FORMAT_D16_UNORM,         DXGI_FORMAT_UNKNOWN             }, // D16
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D24
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D24S8
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D32
		{ DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_D32_FLOAT,         DXGI_FORMAT_UNKNOWN             }, // D16F
		{ DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_D32_FLOAT,         DXGI_FORMAT_UNKNOWN             }, // D24F
		{ DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_D32_FLOAT,         DXGI_FORMAT_UNKNOWN             }, // D32F
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_textureFormat) );

	static const D3D11_INPUT_ELEMENT_DESC s_attrib[] =
	{
		{ "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TANGENT",      0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BITANGENT",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",        0, DXGI_FORMAT_R8G8B8A8_UINT,   0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",        1, DXGI_FORMAT_R8G8B8A8_UINT,   0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,   0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     1, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     2, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     3, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     4, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     5, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     6, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     7, DXGI_FORMAT_R32G32_FLOAT,    0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	BX_STATIC_ASSERT(Attrib::Count == BX_COUNTOF(s_attrib) );

	static const DXGI_FORMAT s_attribType[][4][2] =
	{
		{ // Uint8
			{ DXGI_FORMAT_R8_UINT,            DXGI_FORMAT_R8_UNORM           },
			{ DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_R8G8_UNORM         },
			{ DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R8G8B8A8_UNORM     },
			{ DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R8G8B8A8_UNORM     },
		},
		{ // Uint10
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
		},
		{ // Int16
			{ DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SNORM          },
			{ DXGI_FORMAT_R16G16_SINT,        DXGI_FORMAT_R16G16_SNORM       },
			{ DXGI_FORMAT_R16G16B16A16_SINT,  DXGI_FORMAT_R16G16B16A16_SNORM },
			{ DXGI_FORMAT_R16G16B16A16_SINT,  DXGI_FORMAT_R16G16B16A16_SNORM },
		},
		{ // Half
			{ DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT          },
			{ DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT       },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
		},
		{ // Float
			{ DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT          },
			{ DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT       },
			{ DXGI_FORMAT_R32G32B32_FLOAT,    DXGI_FORMAT_R32G32B32_FLOAT    },
			{ DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT },
		},
	};
	BX_STATIC_ASSERT(AttribType::Count == BX_COUNTOF(s_attribType) );

	static D3D11_INPUT_ELEMENT_DESC* fillVertexDecl(D3D11_INPUT_ELEMENT_DESC* _out, const VertexDecl& _decl)
	{
		D3D11_INPUT_ELEMENT_DESC* elem = _out;

		for (uint32_t attr = 0; attr < Attrib::Count; ++attr)
		{
			if (UINT16_MAX != _decl.m_attributes[attr])
			{
				memcpy(elem, &s_attrib[attr], sizeof(D3D11_INPUT_ELEMENT_DESC) );

				if (0 == _decl.m_attributes[attr])
				{
					elem->AlignedByteOffset = 0;
				}
				else
				{
					uint8_t num;
					AttribType::Enum type;
					bool normalized;
					bool asInt;
					_decl.decode(Attrib::Enum(attr), num, type, normalized, asInt);
					elem->Format = s_attribType[type][num-1][normalized];
					elem->AlignedByteOffset = _decl.m_offset[attr];
				}

				++elem;
			}
		}

		return elem;
	}

	struct TextureStage
	{
		TextureStage()
		{
			clear();
		}

		void clear()
		{
			memset(m_srv, 0, sizeof(m_srv) );
			memset(m_sampler, 0, sizeof(m_sampler) );
		}

		ID3D11ShaderResourceView* m_srv[BGFX_CONFIG_MAX_TEXTURE_SAMPLERS];
		ID3D11SamplerState* m_sampler[BGFX_CONFIG_MAX_TEXTURE_SAMPLERS];
	};

	BX_PRAGMA_DIAGNOSTIC_PUSH();
	BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG("-Wunused-const-variable");
	BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG("-Wunneeded-internal-declaration");

	static const GUID WKPDID_D3DDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
	static const GUID IID_ID3D11Texture2D       = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
	static const GUID IID_IDXGIFactory          = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
	static const GUID IID_IDXGIDevice0          = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
	static const GUID IID_IDXGIDevice1          = { 0x77db970f, 0x6276, 0x48ba, { 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c } };
	static const GUID IID_IDXGIDevice2          = { 0x05008617, 0xfbfd, 0x4051, { 0xa7, 0x90, 0x14, 0x48, 0x84, 0xb4, 0xf6, 0xa9 } };
	static const GUID IID_IDXGIDevice3          = { 0x6007896c, 0x3244, 0x4afd, { 0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23 } };
	static const GUID IID_IDXGIAdapter          = { 0x2411e7e1, 0x12ac, 0x4ccf, { 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0 } };
	static const GUID IID_ID3D11InfoQueue       = { 0x6543dbb6, 0x1b48, 0x42f5, { 0xab, 0x82, 0xe9, 0x7e, 0xc7, 0x43, 0x26, 0xf6 } };
	static const GUID IID_IDXGIDeviceRenderDoc  = { 0xa7aa6116, 0x9c8d, 0x4bba, { 0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78 } };

	enum D3D11_FORMAT_SUPPORT2
	{
		D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD  = 0x40,
		D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE = 0x80,
	};

	static const GUID s_deviceIIDs[] =
	{
		IID_IDXGIDevice3,
		IID_IDXGIDevice2,
		IID_IDXGIDevice1,
		IID_IDXGIDevice0,
	};

	template <typename Ty>
	static BX_NO_INLINE void setDebugObjectName(Ty* _interface, const char* _format, ...)
	{
		if (BX_ENABLED(BGFX_CONFIG_DEBUG_OBJECT_NAME) )
		{
			char temp[2048];
			va_list argList;
			va_start(argList, _format);
			int size = bx::uint32_min(sizeof(temp)-1, vsnprintf(temp, sizeof(temp), _format, argList) );
			va_end(argList);
			temp[size] = '\0';

			_interface->SetPrivateData(WKPDID_D3DDebugObjectName, size, temp);
		}
	}

	BX_PRAGMA_DIAGNOSTIC_POP();

	static BX_NO_INLINE bool getIntelExtensions(ID3D11Device* _device)
	{
		uint8_t temp[28];

		D3D11_BUFFER_DESC desc;
		desc.ByteWidth = sizeof(temp);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;

		D3D11_SUBRESOURCE_DATA initData;
		initData.pSysMem = &temp;
		initData.SysMemPitch = sizeof(temp);
		initData.SysMemSlicePitch = 0;

		bx::StaticMemoryBlockWriter writer(&temp, sizeof(temp) );
		bx::write(&writer, "INTCEXTNCAPSFUNC", 16);
		bx::write(&writer, UINT32_C(0x00010000) );
		bx::write(&writer, UINT32_C(0) );
		bx::write(&writer, UINT32_C(0) );

		ID3D11Buffer* buffer;
		HRESULT hr = _device->CreateBuffer(&desc, &initData, &buffer);

		if (SUCCEEDED(hr) )
		{
			buffer->Release();

			bx::MemoryReader reader(&temp, sizeof(temp) );
			bx::skip(&reader, 16);

			uint32_t version;
			bx::read(&reader, version);

			uint32_t driverVersion;
			bx::read(&reader, driverVersion);

			return version <= driverVersion;
		}

		return false;
	};

	enum AGS_RETURN_CODE
	{
		AGS_SUCCESS,
		AGS_INVALID_ARGS,
		AGS_OUT_OF_MEMORY,
		AGS_ERROR_MISSING_DLL,
		AGS_ERROR_LEGACY_DRIVER,
		AGS_EXTENSION_NOT_SUPPORTED,
		AGS_ADL_FAILURE,
	};

	enum AGS_DRIVER_EXTENSION
	{
		AGS_EXTENSION_QUADLIST          = 1 << 0,
		AGS_EXTENSION_UAV_OVERLAP       = 1 << 1,
		AGS_EXTENSION_DEPTH_BOUNDS_TEST = 1 << 2,
		AGS_EXTENSION_MULTIDRAWINDIRECT = 1 << 3,
	};

	struct AGSDriverVersionInfo
	{
		char strDriverVersion[256];
		char strCatalystVersion[256];
		char strCatalystWebLink[256];
	};

	struct AGSContext;

	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_INIT)(AGSContext**);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_DEINIT)(AGSContext*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_GET_CROSSFIRE_GPU_COUNT)(AGSContext*, int32_t*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_GET_TOTAL_GPU_COUNT)(AGSContext*, int32_t*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_GET_GPU_MEMORY_SIZE)(AGSContext*, int32_t, int64_t*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_GET_DRIVER_VERSION_INFO)(AGSContext*, AGSDriverVersionInfo*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_DRIVER_EXTENSIONS_INIT)(AGSContext*, ID3D11Device*, uint32_t*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_DRIVER_EXTENSIONS_DEINIT)(AGSContext*);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_DRIVER_EXTENSIONS_MULTIDRAW_INSTANCED_INDIRECT)(AGSContext*, uint32_t, ID3D11Buffer*, uint32_t, uint32_t);
	typedef AGS_RETURN_CODE (__cdecl* PFN_AGS_DRIVER_EXTENSIONS_MULTIDRAW_INDEXED_INSTANCED_INDIRECT)(AGSContext*, uint32_t, ID3D11Buffer*, uint32_t, uint32_t);

	static PFN_AGS_INIT   agsInit;
	static PFN_AGS_DEINIT agsDeInit;
	static PFN_AGS_GET_CROSSFIRE_GPU_COUNT  agsGetCrossfireGPUCount;
	static PFN_AGS_GET_TOTAL_GPU_COUNT      agsGetTotalGPUCount;
	static PFN_AGS_GET_GPU_MEMORY_SIZE      agsGetGPUMemorySize;
	static PFN_AGS_GET_DRIVER_VERSION_INFO  agsGetDriverVersionInfo;
	static PFN_AGS_DRIVER_EXTENSIONS_INIT   agsDriverExtensions_Init;
	static PFN_AGS_DRIVER_EXTENSIONS_DEINIT agsDriverExtensions_DeInit;
	static PFN_AGS_DRIVER_EXTENSIONS_MULTIDRAW_INSTANCED_INDIRECT         agsDriverExtensions_MultiDrawInstancedIndirect;
	static PFN_AGS_DRIVER_EXTENSIONS_MULTIDRAW_INDEXED_INSTANCED_INDIRECT agsDriverExtensions_MultiDrawIndexedInstancedIndirect;

	typedef void (* MultiDrawIndirectFn)(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride);

	void stubMultiDrawInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride);
	void stubMultiDrawIndexedInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride);

	void amdAgsMultiDrawInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride);
	void amdAgsMultiDrawIndexedInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride);

	static MultiDrawIndirectFn multiDrawInstancedIndirect;
	static MultiDrawIndirectFn multiDrawIndexedInstancedIndirect;

#if USE_D3D11_DYNAMIC_LIB
	static PFN_D3D11_CREATE_DEVICE  D3D11CreateDevice;
	static PFN_CREATE_DXGI_FACTORY  CreateDXGIFactory;
	static PFN_D3DPERF_SET_MARKER   D3DPERF_SetMarker;
	static PFN_D3DPERF_BEGIN_EVENT  D3DPERF_BeginEvent;
	static PFN_D3DPERF_END_EVENT    D3DPERF_EndEvent;
	static PFN_GET_DEBUG_INTERFACE  DXGIGetDebugInterface;
	static PFN_GET_DEBUG_INTERFACE1 DXGIGetDebugInterface1;
#endif // USE_D3D11_DYNAMIC_LIB

	struct RendererContextD3D11 : public RendererContextI
	{
		RendererContextD3D11()
			: m_d3d9dll(NULL)
			, m_d3d11dll(NULL)
			, m_dxgidll(NULL)
			, m_dxgidebugdll(NULL)
			, m_renderdocdll(NULL)
			, m_agsdll(NULL)
			, m_ags(NULL)
			, m_driverType(D3D_DRIVER_TYPE_NULL)
			, m_featureLevel(D3D_FEATURE_LEVEL(0) )
			, m_adapter(NULL)
			, m_factory(NULL)
			, m_swapChain(NULL)
			, m_lost(0)
			, m_numWindows(0)
			, m_device(NULL)
			, m_deviceCtx(NULL)
			, m_infoQueue(NULL)
			, m_backBufferColor(NULL)
			, m_backBufferDepthStencil(NULL)
			, m_currentColor(NULL)
			, m_currentDepthStencil(NULL)
			, m_captureTexture(NULL)
			, m_captureResolve(NULL)
			, m_wireframe(false)
			, m_flags(BGFX_RESET_NONE)
			, m_maxAnisotropy(1)
			, m_currentProgram(NULL)
			, m_vsChanges(0)
			, m_fsChanges(0)
			, m_rtMsaa(false)
			, m_ovrRtv(NULL)
			, m_ovrDsv(NULL)
		{
			m_fbh.idx = invalidHandle;
			memset(&m_adapterDesc, 0, sizeof(m_adapterDesc) );
			memset(&m_scd, 0, sizeof(m_scd) );
			memset(&m_windows, 0xff, sizeof(m_windows) );
		}

		~RendererContextD3D11()
		{
		}

		bool init()
		{
			struct ErrorState
			{
				enum Enum
				{
					Default,
					LoadedD3D11,
					LoadedDXGI,
					CreatedDXGIFactory,
				};
			};

			ErrorState::Enum errorState = ErrorState::Default;

			// Must be before device creation, and before RenderDoc.
			m_ovr.init();

			if (!m_ovr.isInitialized() )
			{
				m_renderdocdll = loadRenderDoc();
			}

			m_fbh.idx = invalidHandle;
			memset(m_uniforms, 0, sizeof(m_uniforms) );
			memset(&m_resolution, 0, sizeof(m_resolution) );

			m_ags = NULL;
			m_agsdll = bx::dlopen(
#if BX_ARCH_32BIT
						"amd_ags_x86.dll"
#else
						"amd_ags_x64.dll"
#endif // BX_ARCH_32BIT
						);
			if (NULL != m_agsdll)
			{
				agsInit   = (PFN_AGS_INIT  )bx::dlsym(m_agsdll, "agsInit");
				agsDeInit = (PFN_AGS_DEINIT)bx::dlsym(m_agsdll, "agsDeInit");
				agsGetCrossfireGPUCount    = (PFN_AGS_GET_CROSSFIRE_GPU_COUNT )bx::dlsym(m_agsdll, "agsGetCrossfireGPUCount");
				agsGetTotalGPUCount        = (PFN_AGS_GET_TOTAL_GPU_COUNT     )bx::dlsym(m_agsdll, "agsGetTotalGPUCount");
				agsGetGPUMemorySize        = (PFN_AGS_GET_GPU_MEMORY_SIZE     )bx::dlsym(m_agsdll, "agsGetGPUMemorySize");
				agsGetDriverVersionInfo    = (PFN_AGS_GET_DRIVER_VERSION_INFO )bx::dlsym(m_agsdll, "agsGetDriverVersionInfo");
				agsDriverExtensions_Init   = (PFN_AGS_DRIVER_EXTENSIONS_INIT  )bx::dlsym(m_agsdll, "agsDriverExtensions_Init");
				agsDriverExtensions_DeInit = (PFN_AGS_DRIVER_EXTENSIONS_DEINIT)bx::dlsym(m_agsdll, "agsDriverExtensions_DeInit");
				agsDriverExtensions_MultiDrawInstancedIndirect        = (PFN_AGS_DRIVER_EXTENSIONS_MULTIDRAW_INSTANCED_INDIRECT        )bx::dlsym(m_agsdll, "agsDriverExtensions_MultiDrawInstancedIndirect");
				agsDriverExtensions_MultiDrawIndexedInstancedIndirect = (PFN_AGS_DRIVER_EXTENSIONS_MULTIDRAW_INDEXED_INSTANCED_INDIRECT)bx::dlsym(m_agsdll, "agsDriverExtensions_MultiDrawIndexedInstancedIndirect");

				bool agsSupported = true
					&& NULL != agsInit
					&& NULL != agsDeInit
					&& NULL != agsGetCrossfireGPUCount
					&& NULL != agsGetTotalGPUCount
					&& NULL != agsGetGPUMemorySize
					&& NULL != agsGetDriverVersionInfo
					&& NULL != agsDriverExtensions_Init
					&& NULL != agsDriverExtensions_DeInit
					&& NULL != agsDriverExtensions_MultiDrawInstancedIndirect
					&& NULL != agsDriverExtensions_MultiDrawIndexedInstancedIndirect
					;
				if (agsSupported)
				{
					AGS_RETURN_CODE result = agsInit(&m_ags);
					agsSupported = AGS_SUCCESS == result;
					if (agsSupported)
					{
						AGSDriverVersionInfo vi;
						result = agsGetDriverVersionInfo(m_ags, &vi);
						BX_TRACE("      Driver version: %s", vi.strDriverVersion);
						BX_TRACE("    Catalyst version: %s", vi.strCatalystVersion);

						int32_t numCrossfireGPUs = 0;
						result = agsGetCrossfireGPUCount(m_ags, &numCrossfireGPUs);
						BX_TRACE("  Num crossfire GPUs: %d", numCrossfireGPUs);

						int32_t numGPUs = 0;
						result = agsGetTotalGPUCount(m_ags, &numGPUs);
						BX_TRACE("            Num GPUs: %d", numGPUs);

						for (int32_t ii = 0; ii < numGPUs; ++ii)
						{
							long long memSize;
							result = agsGetGPUMemorySize(m_ags, ii, &memSize);
							if (AGS_SUCCESS == result)
							{
								char memSizeStr[16];
								bx::prettify(memSizeStr, BX_COUNTOF(memSizeStr), memSize);
								BX_TRACE("     GPU #%d mem size: %s", ii, memSizeStr);
							}
						}
					}
				}

				BX_WARN(!agsSupported, "AMD/AGS supported.");
				if (!agsSupported)
				{
					if (NULL != m_ags)
					{
						agsDeInit(m_ags);
						m_ags = NULL;
					}

					bx::dlclose(m_agsdll);
					m_agsdll = NULL;
				}
			}

#if USE_D3D11_DYNAMIC_LIB
			m_d3d11dll = bx::dlopen("d3d11.dll");
			BX_WARN(NULL != m_d3d11dll, "Failed to load d3d11.dll.");

			if (NULL == m_d3d11dll)
			{
				goto error;
			}

			errorState = ErrorState::LoadedD3D11;

			m_d3d9dll = NULL;

			if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
			{
				// D3D11_1.h has ID3DUserDefinedAnnotation
				// http://msdn.microsoft.com/en-us/library/windows/desktop/hh446881%28v=vs.85%29.aspx
				m_d3d9dll = bx::dlopen("d3d9.dll");
				if (NULL != m_d3d9dll)
				{
					D3DPERF_SetMarker  = (PFN_D3DPERF_SET_MARKER )bx::dlsym(m_d3d9dll, "D3DPERF_SetMarker" );
					D3DPERF_BeginEvent = (PFN_D3DPERF_BEGIN_EVENT)bx::dlsym(m_d3d9dll, "D3DPERF_BeginEvent");
					D3DPERF_EndEvent   = (PFN_D3DPERF_END_EVENT  )bx::dlsym(m_d3d9dll, "D3DPERF_EndEvent"  );
					BX_CHECK(NULL != D3DPERF_SetMarker
						  && NULL != D3DPERF_BeginEvent
						  && NULL != D3DPERF_EndEvent
						  , "Failed to initialize PIX events."
						  );
				}
			}

			D3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)bx::dlsym(m_d3d11dll, "D3D11CreateDevice");
			BX_WARN(NULL != D3D11CreateDevice, "Function D3D11CreateDevice not found.");
			if (NULL == D3D11CreateDevice)
			{
				goto error;
			}

			m_dxgidll = bx::dlopen("dxgi.dll");
			BX_WARN(NULL != m_dxgidll, "Failed to load dxgi.dll.");
			if (NULL == m_dxgidll)
			{
				goto error;
			}

			errorState = ErrorState::LoadedDXGI;

			CreateDXGIFactory = (PFN_CREATE_DXGI_FACTORY)bx::dlsym(m_dxgidll, "CreateDXGIFactory");
			BX_WARN(NULL != CreateDXGIFactory, "Function CreateDXGIFactory not found.");
			if (NULL == CreateDXGIFactory)
			{
				goto error;
			}

			m_dxgidebugdll = bx::dlopen("dxgidebug.dll");
			if (NULL != m_dxgidebugdll)
			{
				DXGIGetDebugInterface  = (PFN_GET_DEBUG_INTERFACE )bx::dlsym(m_dxgidebugdll, "DXGIGetDebugInterface");
				DXGIGetDebugInterface1 = (PFN_GET_DEBUG_INTERFACE1)bx::dlsym(m_dxgidebugdll, "DXGIGetDebugInterface1");
				if (NULL == DXGIGetDebugInterface
				&&  NULL == DXGIGetDebugInterface1)
				{
					bx::dlclose(m_dxgidebugdll);
					m_dxgidebugdll = NULL;
				}
				else
				{
					// Figure out how to access IDXGIInfoQueue on pre Win8...
				}
			}
#endif // USE_D3D11_DYNAMIC_LIB

			HRESULT hr;
			IDXGIFactory* factory;
#if BX_PLATFORM_WINRT
			// WinRT requires the IDXGIFactory2 interface, which isn't supported on older platforms
			hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory);
#else
			hr = CreateDXGIFactory(IID_IDXGIFactory, (void**)&factory);
#endif // BX_PLATFORM_WINRT
			BX_WARN(SUCCEEDED(hr), "Unable to create DXGI factory.");
			if (FAILED(hr) )
			{
				goto error;
			}

			errorState = ErrorState::CreatedDXGIFactory;

			m_device = (ID3D11Device*)g_platformData.context;

			if (NULL == m_device)
			{
				m_adapter    = NULL;
				m_driverType = BGFX_PCI_ID_SOFTWARE_RASTERIZER == g_caps.vendorId
					? D3D_DRIVER_TYPE_WARP
					: D3D_DRIVER_TYPE_HARDWARE
					;

				IDXGIAdapter* adapter;
				for (uint32_t ii = 0
					; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters(ii, &adapter) && ii < BX_COUNTOF(g_caps.gpu)
					; ++ii
					)
				{
					DXGI_ADAPTER_DESC desc;
					hr = adapter->GetDesc(&desc);
					if (SUCCEEDED(hr) )
					{
						BX_TRACE("Adapter #%d", ii);

						char description[BX_COUNTOF(desc.Description)];
						wcstombs(description, desc.Description, BX_COUNTOF(desc.Description) );
						BX_TRACE("\tDescription: %s", description);
						BX_TRACE("\tVendorId: 0x%08x, DeviceId: 0x%08x, SubSysId: 0x%08x, Revision: 0x%08x"
							, desc.VendorId
							, desc.DeviceId
							, desc.SubSysId
							, desc.Revision
							);
						BX_TRACE("\tMemory: %" PRIi64 " (video), %" PRIi64 " (system), %" PRIi64 " (shared)"
							, desc.DedicatedVideoMemory
							, desc.DedicatedSystemMemory
							, desc.SharedSystemMemory
							);

						g_caps.gpu[ii].vendorId = (uint16_t)desc.VendorId;
						g_caps.gpu[ii].deviceId = (uint16_t)desc.DeviceId;
						++g_caps.numGPUs;

						if (NULL == m_adapter)
						{
							if ( (BGFX_PCI_ID_NONE != g_caps.vendorId ||             0 != g_caps.deviceId)
							&&   (BGFX_PCI_ID_NONE == g_caps.vendorId || desc.VendorId == g_caps.vendorId)
							&&   (               0 == g_caps.deviceId || desc.DeviceId == g_caps.deviceId) )
							{
								m_adapter = adapter;
								m_adapter->AddRef();
								m_driverType = D3D_DRIVER_TYPE_UNKNOWN;
							}

							if (BX_ENABLED(BGFX_CONFIG_DEBUG_PERFHUD)
							&&  0 != strstr(description, "PerfHUD") )
							{
								m_adapter = adapter;
								m_driverType = D3D_DRIVER_TYPE_REFERENCE;
							}
						}
					}

					DX_RELEASE(adapter, adapter == m_adapter ? 1 : 0);
				}
				DX_RELEASE(factory, NULL != m_adapter ? 1 : 0);

				D3D_FEATURE_LEVEL featureLevel[] =
				{
					D3D_FEATURE_LEVEL_11_1,
					D3D_FEATURE_LEVEL_11_0,
					D3D_FEATURE_LEVEL_10_1,
					D3D_FEATURE_LEVEL_10_0,
					D3D_FEATURE_LEVEL_9_3,
					D3D_FEATURE_LEVEL_9_2,
					D3D_FEATURE_LEVEL_9_1,
				};

				for (;;)
				{
					uint32_t flags = 0
						| D3D11_CREATE_DEVICE_SINGLETHREADED
						| D3D11_CREATE_DEVICE_BGRA_SUPPORT
//						| D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS
						| (BX_ENABLED(BGFX_CONFIG_DEBUG) ? D3D11_CREATE_DEVICE_DEBUG : 0)
						;

					hr = E_FAIL;
					for (uint32_t ii = 0; ii < 3 && FAILED(hr);)
					{
						hr = D3D11CreateDevice(m_adapter
							, m_driverType
							, NULL
							, flags
							, &featureLevel[ii]
							, BX_COUNTOF(featureLevel)-ii
							, D3D11_SDK_VERSION
							, &m_device
							, &m_featureLevel
							, &m_deviceCtx
							);
						BX_WARN(FAILED(hr), "Direct3D11 device feature level %d.%d."
							, (m_featureLevel >> 12) & 0xf
							, (m_featureLevel >>  8) & 0xf
							);
						if (FAILED(hr)
						&&  0 != (flags & D3D11_CREATE_DEVICE_DEBUG) )
						{
							// Try without debug in case D3D11 SDK Layers
							// is not present?
							flags &= ~D3D11_CREATE_DEVICE_DEBUG;
							continue;
						}

						// Enable debug flags.
						flags |= (BX_ENABLED(BGFX_CONFIG_DEBUG) ? D3D11_CREATE_DEVICE_DEBUG : 0);
						++ii;
					}

					if (FAILED(hr)
					&&  D3D_DRIVER_TYPE_WARP != m_driverType)
					{
						// Try with WARP
						m_driverType = D3D_DRIVER_TYPE_WARP;
						continue;
					}

					break;
				}
				BX_WARN(SUCCEEDED(hr), "Unable to create Direct3D11 device.");

				if (FAILED(hr) )
				{
					goto error;
				}

				if (NULL != m_adapter)
				{
					DX_RELEASE(m_adapter, 2);
				}
			}
			else
			{
				m_device->GetImmediateContext(&m_deviceCtx);
				BX_WARN(NULL != m_deviceCtx, "Unable to create Direct3D11 device.");

				if (NULL == m_deviceCtx)
				{
					goto error;
				}
			}

			{
				IDXGIDevice*  device = NULL;
				IDXGIAdapter* adapter = NULL;
				hr = E_FAIL;
				for (uint32_t ii = 0; ii < BX_COUNTOF(s_deviceIIDs) && FAILED(hr); ++ii)
				{
					hr = m_device->QueryInterface(s_deviceIIDs[ii], (void**)&device);
					BX_TRACE("D3D device 11.%d, hr %x", BX_COUNTOF(s_deviceIIDs)-1-ii, hr);

					if (SUCCEEDED(hr) )
					{
#if BX_COMPILER_MSVC
BX_PRAGMA_DIAGNOSTIC_PUSH();
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4530) // warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc
						try
						{
							// QueryInterface above can succeed, but getting adapter call might crash on Win7.
							hr = device->GetAdapter(&adapter);
						}
						catch (...)
						{
							BX_TRACE("Failed to get adapter foro IID_IDXGIDevice%d.", BX_COUNTOF(s_deviceIIDs)-1-ii);
							DX_RELEASE(device, 0);
							hr = E_FAIL;
						}
BX_PRAGMA_DIAGNOSTIC_POP();
#else
						hr = device->GetAdapter(&adapter);
#endif // BX_COMPILER_MSVC
					}
				}

				BX_WARN(SUCCEEDED(hr), "Unable to create Direct3D11 device.");
				if (FAILED(hr) )
				{
					goto error;
				}

				// GPA increases device ref count.
				// RenderDoc makes device ref count 0 here.
				//
				// This causes assert in debug. When debugger is present refcount
				// checks are off.
				IDXGIDevice* renderdoc;
				hr = m_device->QueryInterface(IID_IDXGIDeviceRenderDoc, (void**)&renderdoc);
				if (SUCCEEDED(hr) )
				{
					setGraphicsDebuggerPresent(true);
					DX_RELEASE(renderdoc, 2);
				}
				else
				{
					setGraphicsDebuggerPresent(3 != getRefCount(device) );
					DX_RELEASE(device, 2);
				}

				hr = adapter->GetDesc(&m_adapterDesc);
				BX_WARN(SUCCEEDED(hr), "Unable to create Direct3D11 device.");
				if (FAILED(hr) )
				{
					DX_RELEASE(adapter, 2);
					goto error;
				}

				g_caps.vendorId = 0 == m_adapterDesc.VendorId
					? BGFX_PCI_ID_SOFTWARE_RASTERIZER
					: (uint16_t)m_adapterDesc.VendorId
					;
				g_caps.deviceId = (uint16_t)m_adapterDesc.DeviceId;

				if (NULL == g_platformData.backBuffer)
				{
#if BX_PLATFORM_WINRT
					hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&m_factory);
					BX_WARN(SUCCEEDED(hr), "Unable to create Direct3D11 device.");
					DX_RELEASE(adapter, 2);
					if (FAILED(hr) )
					{
						goto error;
					}

					memset(&m_scd, 0, sizeof(m_scd) );
					m_scd.Width  = BGFX_DEFAULT_WIDTH;
					m_scd.Height = BGFX_DEFAULT_HEIGHT;
					m_scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					m_scd.Stereo = false;
					m_scd.SampleDesc.Count   = 1;
					m_scd.SampleDesc.Quality = 0;
					m_scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					m_scd.BufferCount = 2;
					m_scd.Scaling     = DXGI_SCALING_NONE;
					m_scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
					m_scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

					hr = m_factory->CreateSwapChainForCoreWindow(m_device
						, (::IUnknown*)g_platformData.nwh
						, &m_scd
						, NULL
						, &m_swapChain
						);
#else
					hr = adapter->GetParent(IID_IDXGIFactory, (void**)&m_factory);
					BX_WARN(SUCCEEDED(hr), "Unable to create Direct3D11 device.");
					DX_RELEASE(adapter, 2);
					if (FAILED(hr) )
					{
						goto error;
					}

					memset(&m_scd, 0, sizeof(m_scd) );
					m_scd.BufferDesc.Width  = BGFX_DEFAULT_WIDTH;
					m_scd.BufferDesc.Height = BGFX_DEFAULT_HEIGHT;
					m_scd.BufferDesc.RefreshRate.Numerator   = 60;
					m_scd.BufferDesc.RefreshRate.Denominator = 1;
					m_scd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
					m_scd.SampleDesc.Count   = 1;
					m_scd.SampleDesc.Quality = 0;
					m_scd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					m_scd.BufferCount  = 1;
					m_scd.OutputWindow = (HWND)g_platformData.nwh;
					m_scd.Windowed     = true;

					hr = m_factory->CreateSwapChain(m_device
						, &m_scd
						, &m_swapChain
						);

					DX_CHECK(m_factory->MakeWindowAssociation( (HWND)g_platformData.nwh, 0
						| DXGI_MWA_NO_WINDOW_CHANGES
						| DXGI_MWA_NO_ALT_ENTER
						) );
#endif // BX_PLATFORM_WINRT
					BX_WARN(SUCCEEDED(hr), "Failed to create swap chain.");
					if (FAILED(hr) )
					{
						goto error;
					}
				}
				else
				{
					memset(&m_scd, 0, sizeof(m_scd) );
					m_scd.SampleDesc.Count   = 1;
					m_scd.SampleDesc.Quality = 0;
					setBufferSize(BGFX_DEFAULT_WIDTH, BGFX_DEFAULT_HEIGHT);
					m_backBufferColor        = (ID3D11RenderTargetView*)g_platformData.backBuffer;
					m_backBufferDepthStencil = (ID3D11DepthStencilView*)g_platformData.backBufferDS;
				}
			}

			m_numWindows = 1;

			if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
			{
				hr = m_device->QueryInterface(IID_ID3D11InfoQueue, (void**)&m_infoQueue);

				if (SUCCEEDED(hr) )
				{
					m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
					m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR,      true);
					m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING,    false);

					D3D11_INFO_QUEUE_FILTER filter;
					memset(&filter, 0, sizeof(filter) );

					D3D11_MESSAGE_CATEGORY catlist[] =
					{
						D3D11_MESSAGE_CATEGORY_STATE_CREATION,
					};
					filter.DenyList.NumCategories = BX_COUNTOF(catlist);
					filter.DenyList.pCategoryList = catlist;
					m_infoQueue->PushStorageFilter(&filter);

					DX_RELEASE(m_infoQueue, 3);
				}
			}

			{

				UniformHandle handle = BGFX_INVALID_HANDLE;
				for (uint32_t ii = 0; ii < PredefinedUniform::Count; ++ii)
				{
					m_uniformReg.add(handle, getPredefinedUniformName(PredefinedUniform::Enum(ii) ), &m_predefinedUniforms[ii]);
				}

				g_caps.supported |= (0
					| BGFX_CAPS_TEXTURE_3D
					| BGFX_CAPS_VERTEX_ATTRIB_HALF
					| BGFX_CAPS_VERTEX_ATTRIB_UINT10
					| BGFX_CAPS_FRAGMENT_DEPTH
					| (getIntelExtensions(m_device) ? BGFX_CAPS_FRAGMENT_ORDERING : 0)
					| BGFX_CAPS_SWAP_CHAIN
					| (m_ovr.isInitialized() ? BGFX_CAPS_HMD : 0)
					| BGFX_CAPS_DRAW_INDIRECT
					);

				if (m_featureLevel <= D3D_FEATURE_LEVEL_9_2)
				{
					g_caps.maxTextureSize   = D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;
					g_caps.maxFBAttachments = uint8_t(bx::uint32_min(D3D_FL9_1_SIMULTANEOUS_RENDER_TARGET_COUNT, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) );
				}
				else if (m_featureLevel == D3D_FEATURE_LEVEL_9_3)
				{
					g_caps.maxTextureSize   = D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;
					g_caps.maxFBAttachments = uint8_t(bx::uint32_min(D3D_FL9_3_SIMULTANEOUS_RENDER_TARGET_COUNT, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) );
				}
				else
				{
					g_caps.supported |= BGFX_CAPS_TEXTURE_COMPARE_ALL;
					g_caps.maxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
					g_caps.maxFBAttachments = uint8_t(bx::uint32_min(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) );
				}

				// 32-bit indices only supported on 9_2+.
				if (m_featureLevel >= D3D_FEATURE_LEVEL_9_2)
				{
					g_caps.supported |= BGFX_CAPS_INDEX32;
				}

				// Independent blend only supported on 10_1+.
				if (m_featureLevel >= D3D_FEATURE_LEVEL_10_1)
				{
					g_caps.supported |= BGFX_CAPS_BLEND_INDEPENDENT;
				}

				// Compute support is optional on 10_0 and 10_1 targets.
				if (m_featureLevel == D3D_FEATURE_LEVEL_10_0
				||  m_featureLevel == D3D_FEATURE_LEVEL_10_1)
				{
					struct D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS
					{
						BOOL ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
					};

					D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS data;
					hr = m_device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &data, sizeof(data) );
					if (SUCCEEDED(hr)
					&&  data.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
					{
						g_caps.supported |= BGFX_CAPS_COMPUTE;
					}
				}
				else if (m_featureLevel >= D3D_FEATURE_LEVEL_11_0)
				{
					g_caps.supported |= BGFX_CAPS_COMPUTE;
				}

				// Instancing fully supported on 9_3+, optionally partially supported at lower levels.
				if (m_featureLevel >= D3D_FEATURE_LEVEL_9_3)
				{
					g_caps.supported |= BGFX_CAPS_INSTANCING;
				}
				else
				{
					struct D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT
					{
						BOOL SimpleInstancingSupported;
					};

					D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT data;
					hr = m_device->CheckFeatureSupport(D3D11_FEATURE(11) /*D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT*/, &data, sizeof(data) );
					if (SUCCEEDED(hr)
					&&  data.SimpleInstancingSupported)
					{
						g_caps.supported |= BGFX_CAPS_INSTANCING;
					}
				}

				// shadow compare is optional on 9_1 through 9_3 targets
				if (m_featureLevel <= D3D_FEATURE_LEVEL_9_3)
				{
					struct D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT
					{
						BOOL SupportsDepthAsTextureWithLessEqualComparisonFilter;
					};

					D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT data;
					hr = m_device->CheckFeatureSupport(D3D11_FEATURE(9) /*D3D11_FEATURE_D3D9_SHADOW_SUPPORT*/, &data, sizeof(data) );
					if (SUCCEEDED(hr)
					&&  data.SupportsDepthAsTextureWithLessEqualComparisonFilter)
					{
						g_caps.supported |= BGFX_CAPS_TEXTURE_COMPARE_LEQUAL;
					}
				}

				for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
				{
					uint8_t support = BGFX_CAPS_FORMAT_TEXTURE_NONE;

					const DXGI_FORMAT fmt = isDepth(TextureFormat::Enum(ii) )
						? s_textureFormat[ii].m_fmtDsv
						: s_textureFormat[ii].m_fmt
						;
					const DXGI_FORMAT fmtSrgb = s_textureFormat[ii].m_fmtSrgb;

					if (DXGI_FORMAT_UNKNOWN != fmt)
					{
						struct D3D11_FEATURE_DATA_FORMAT_SUPPORT
						{
							DXGI_FORMAT InFormat;
							UINT OutFormatSupport;
						};

						D3D11_FEATURE_DATA_FORMAT_SUPPORT data; // D3D11_FEATURE_DATA_FORMAT_SUPPORT2
						data.InFormat = fmt;
						hr = m_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &data, sizeof(data) );
						if (SUCCEEDED(hr) )
						{
							support |= 0 != (data.OutFormatSupport & (0
									| D3D11_FORMAT_SUPPORT_TEXTURE2D
									| D3D11_FORMAT_SUPPORT_TEXTURE3D
									| D3D11_FORMAT_SUPPORT_TEXTURECUBE
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_COLOR
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;

							support |= 0 != (data.OutFormatSupport & (0
									| D3D11_FORMAT_SUPPORT_BUFFER
									| D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER
									| D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_VERTEX
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;

							support |= 0 != (data.OutFormatSupport & (0
									| D3D11_FORMAT_SUPPORT_SHADER_LOAD
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_IMAGE
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;

							support |= 0 != (data.OutFormatSupport & (0
									| D3D11_FORMAT_SUPPORT_RENDER_TARGET
									| D3D11_FORMAT_SUPPORT_DEPTH_STENCIL
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;
						}
						else
						{
							BX_TRACE("CheckFeatureSupport failed with %x for format %s.", hr, getName(TextureFormat::Enum(ii) ) );
						}

						if (0 != (support & BGFX_CAPS_FORMAT_TEXTURE_IMAGE) )
						{
							// clear image flag for additional testing
							support &= ~BGFX_CAPS_FORMAT_TEXTURE_IMAGE;

							data.InFormat = s_textureFormat[ii].m_fmt;
							hr = m_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &data, sizeof(data) );
							if (SUCCEEDED(hr) )
							{
								support |= 0 != (data.OutFormatSupport & (0
										| D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD
										| D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE
										) )
										? BGFX_CAPS_FORMAT_TEXTURE_IMAGE
										: BGFX_CAPS_FORMAT_TEXTURE_NONE
										;
							}
						}
					}

					if (DXGI_FORMAT_UNKNOWN != fmtSrgb)
					{
						struct D3D11_FEATURE_DATA_FORMAT_SUPPORT
						{
							DXGI_FORMAT InFormat;
							UINT OutFormatSupport;
						};

						D3D11_FEATURE_DATA_FORMAT_SUPPORT data; // D3D11_FEATURE_DATA_FORMAT_SUPPORT2
						data.InFormat = fmtSrgb;
						hr = m_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &data, sizeof(data) );
						if (SUCCEEDED(hr) )
						{
							support |= 0 != (data.OutFormatSupport & (0
									| D3D11_FORMAT_SUPPORT_TEXTURE2D
									| D3D11_FORMAT_SUPPORT_TEXTURE3D
									| D3D11_FORMAT_SUPPORT_TEXTURECUBE
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_COLOR_SRGB
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;
						}
						else
						{
							BX_TRACE("CheckFeatureSupport failed with %x for sRGB format %s.", hr, getName(TextureFormat::Enum(ii) ) );
						}
					}

					g_caps.formats[ii] = support;
				}

				// Init reserved part of view name.
				for (uint32_t ii = 0; ii < BGFX_CONFIG_MAX_VIEWS; ++ii)
				{
					char name[BGFX_CONFIG_MAX_VIEW_NAME_RESERVED+1];
					bx::snprintf(name, sizeof(name), "%3d   ", ii);
					mbstowcs(s_viewNameW[ii], name, BGFX_CONFIG_MAX_VIEW_NAME_RESERVED);
				}

				if (BX_ENABLED(BGFX_CONFIG_DEBUG)
				&&  NULL != m_infoQueue)
				{
					m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);
				}

				{ //
					multiDrawInstancedIndirect        = stubMultiDrawInstancedIndirect;
					multiDrawIndexedInstancedIndirect = stubMultiDrawIndexedInstancedIndirect;
					if (NULL != m_ags)
					{
						uint32_t flags;
						AGS_RETURN_CODE result = agsDriverExtensions_Init(m_ags, m_device, &flags);
						bool hasExtensions = AGS_SUCCESS == result;

						if (hasExtensions
						&&  0 != (flags & AGS_EXTENSION_MULTIDRAWINDIRECT) )
						{
							multiDrawInstancedIndirect        = amdAgsMultiDrawInstancedIndirect;
							multiDrawIndexedInstancedIndirect = amdAgsMultiDrawIndexedInstancedIndirect;
						}
						else
						{
							if (hasExtensions)
							{
								agsDriverExtensions_DeInit(m_ags);
							}

							agsDeInit(m_ags);
							m_ags = NULL;
						}
					}
				}

				//
				updateMsaa();
				postReset();
			}

			return true;

		error:
			switch (errorState)
			{
			case ErrorState::CreatedDXGIFactory:
				DX_RELEASE(m_swapChain, 0);
				DX_RELEASE(m_deviceCtx, 0);
				DX_RELEASE(m_device, 0);
				DX_RELEASE(m_factory, 0);

			case ErrorState::LoadedDXGI:
#if USE_D3D11_DYNAMIC_LIB
				if (NULL != m_dxgidebugdll)
				{
					bx::dlclose(m_dxgidebugdll);
					m_dxgidebugdll = NULL;
				}

				if (NULL != m_d3d9dll)
				{
					bx::dlclose(m_d3d9dll);
					m_d3d9dll = NULL;
				}

				bx::dlclose(m_dxgidll);
				m_dxgidll = NULL;
#endif // USE_D3D11_DYNAMIC_LIB

			case ErrorState::LoadedD3D11:
#if USE_D3D11_DYNAMIC_LIB
				bx::dlclose(m_d3d11dll);
				m_d3d11dll = NULL;
#endif // USE_D3D11_DYNAMIC_LIB

			case ErrorState::Default:
				if (NULL != m_ags)
				{
					agsDeInit(m_ags);
				}
				bx::dlclose(m_agsdll);
				m_agsdll = NULL;
				unloadRenderDoc(m_renderdocdll);
				m_ovr.shutdown();
				break;
			}

			return false;
		}

		void shutdown()
		{
			preReset();
			m_ovr.shutdown();

			if (NULL != m_ags)
			{
				agsDeInit(m_ags);
				m_ags = NULL;
			}

			bx::dlclose(m_agsdll);
			m_agsdll = NULL;

			m_deviceCtx->ClearState();

			invalidateCache();

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_indexBuffers); ++ii)
			{
				m_indexBuffers[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_vertexBuffers); ++ii)
			{
				m_vertexBuffers[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_shaders); ++ii)
			{
				m_shaders[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_textures); ++ii)
			{
				m_textures[ii].destroy();
			}

			DX_RELEASE(m_swapChain, 0);
			DX_RELEASE(m_deviceCtx, 0);
			DX_RELEASE(m_device, 0);
			DX_RELEASE(m_factory, 0);

			unloadRenderDoc(m_renderdocdll);

#if USE_D3D11_DYNAMIC_LIB
			if (NULL != m_dxgidebugdll)
			{
				bx::dlclose(m_dxgidebugdll);
				m_dxgidebugdll = NULL;
			}

			if (NULL != m_d3d9dll)
			{
				bx::dlclose(m_d3d9dll);
				m_d3d9dll = NULL;
			}

			bx::dlclose(m_dxgidll);
			m_dxgidll = NULL;

			bx::dlclose(m_d3d11dll);
			m_d3d11dll = NULL;
#endif // USE_D3D11_DYNAMIC_LIB
		}

		RendererType::Enum getRendererType() const BX_OVERRIDE
		{
			return RendererType::Direct3D11;
		}

		const char* getRendererName() const BX_OVERRIDE
		{
			return BGFX_RENDERER_DIRECT3D11_NAME;
		}

		void createIndexBuffer(IndexBufferHandle _handle, Memory* _mem, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_mem->size, _mem->data, _flags);
		}

		void destroyIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createVertexDecl(VertexDeclHandle _handle, const VertexDecl& _decl) BX_OVERRIDE
		{
			VertexDecl& decl = m_vertexDecls[_handle.idx];
			memcpy(&decl, &_decl, sizeof(VertexDecl) );
			dump(decl);
		}

		void destroyVertexDecl(VertexDeclHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createVertexBuffer(VertexBufferHandle _handle, Memory* _mem, VertexDeclHandle _declHandle, uint16_t _flags) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].create(_mem->size, _mem->data, _declHandle, _flags);
		}

		void destroyVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_size, NULL, _flags);
		}

		void updateDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].update(_offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			VertexDeclHandle decl = BGFX_INVALID_HANDLE;
			m_vertexBuffers[_handle.idx].create(_size, NULL, decl, _flags);
		}

		void updateDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].update(_offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createShader(ShaderHandle _handle, Memory* _mem) BX_OVERRIDE
		{
			m_shaders[_handle.idx].create(_mem);
		}

		void destroyShader(ShaderHandle _handle) BX_OVERRIDE
		{
			m_shaders[_handle.idx].destroy();
		}

		void createProgram(ProgramHandle _handle, ShaderHandle _vsh, ShaderHandle _fsh) BX_OVERRIDE
		{
			m_program[_handle.idx].create(&m_shaders[_vsh.idx], isValid(_fsh) ? &m_shaders[_fsh.idx] : NULL);
		}

		void destroyProgram(ProgramHandle _handle) BX_OVERRIDE
		{
			m_program[_handle.idx].destroy();
		}

		void createTexture(TextureHandle _handle, Memory* _mem, uint32_t _flags, uint8_t _skip) BX_OVERRIDE
		{
			m_textures[_handle.idx].create(_mem, _flags, _skip);
		}

		void updateTextureBegin(TextureHandle /*_handle*/, uint8_t /*_side*/, uint8_t /*_mip*/) BX_OVERRIDE
		{
		}

		void updateTexture(TextureHandle _handle, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem) BX_OVERRIDE
		{
			m_textures[_handle.idx].update(_side, _mip, _rect, _z, _depth, _pitch, _mem);
		}

		void updateTextureEnd() BX_OVERRIDE
		{
		}

		void resizeTexture(TextureHandle _handle, uint16_t _width, uint16_t _height) BX_OVERRIDE
		{
			TextureD3D11& texture = m_textures[_handle.idx];

			uint32_t size = sizeof(uint32_t) + sizeof(TextureCreate);
			const Memory* mem = alloc(size);

			bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
			uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
			bx::write(&writer, magic);

			TextureCreate tc;
			tc.m_flags   = texture.m_flags;
			tc.m_width   = _width;
			tc.m_height  = _height;
			tc.m_sides   = 0;
			tc.m_depth   = 0;
			tc.m_numMips = 1;
			tc.m_format  = texture.m_requestedFormat;
			tc.m_cubeMap = false;
			tc.m_mem     = NULL;
			bx::write(&writer, tc);

			texture.destroy();
			texture.create(mem, tc.m_flags, 0);

			release(mem);
		}

		void destroyTexture(TextureHandle _handle) BX_OVERRIDE
		{
			m_textures[_handle.idx].destroy();
		}

		void createFrameBuffer(FrameBufferHandle _handle, uint8_t _num, const TextureHandle* _textureHandles) BX_OVERRIDE
		{
			m_frameBuffers[_handle.idx].create(_num, _textureHandles);
		}

		void createFrameBuffer(FrameBufferHandle _handle, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat) BX_OVERRIDE
		{
			uint16_t denseIdx = m_numWindows++;
			m_windows[denseIdx] = _handle;
			m_frameBuffers[_handle.idx].create(denseIdx, _nwh, _width, _height, _depthFormat);
		}

		void destroyFrameBuffer(FrameBufferHandle _handle) BX_OVERRIDE
		{
			uint16_t denseIdx = m_frameBuffers[_handle.idx].destroy();
			if (UINT16_MAX != denseIdx)
			{
				--m_numWindows;
				if (m_numWindows > 1)
				{
					FrameBufferHandle handle = m_windows[m_numWindows];
					m_windows[denseIdx] = handle;
					m_frameBuffers[handle.idx].m_denseIdx = denseIdx;
				}
			}
		}

		void createUniform(UniformHandle _handle, UniformType::Enum _type, uint16_t _num, const char* _name) BX_OVERRIDE
		{
			if (NULL != m_uniforms[_handle.idx])
			{
				BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			}

			uint32_t size = BX_ALIGN_16(g_uniformTypeSize[_type]*_num);
			void* data = BX_ALLOC(g_allocator, size);
			memset(data, 0, size);
			m_uniforms[_handle.idx] = data;
			m_uniformReg.add(_handle, _name, data);
		}

		void destroyUniform(UniformHandle _handle) BX_OVERRIDE
		{
			BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			m_uniforms[_handle.idx] = NULL;
		}

		void saveScreenShot(const char* _filePath) BX_OVERRIDE
		{
			BX_WARN(NULL != m_swapChain, "Unable to capture screenshot %s.", _filePath);
			if (NULL == m_swapChain)
			{
				return;
			}

			ID3D11Texture2D* backBuffer;
			DX_CHECK(m_swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&backBuffer) );

			D3D11_TEXTURE2D_DESC backBufferDesc;
			backBuffer->GetDesc(&backBufferDesc);

			D3D11_TEXTURE2D_DESC desc;
			memcpy(&desc, &backBufferDesc, sizeof(desc) );
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			ID3D11Texture2D* texture;
			HRESULT hr = m_device->CreateTexture2D(&desc, NULL, &texture);
			if (SUCCEEDED(hr) )
			{
				if (backBufferDesc.SampleDesc.Count == 1)
				{
					m_deviceCtx->CopyResource(texture, backBuffer);
				}
				else
				{
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.CPUAccessFlags = 0;
					ID3D11Texture2D* resolve;
					hr = m_device->CreateTexture2D(&desc, NULL, &resolve);
					if (SUCCEEDED(hr) )
					{
						m_deviceCtx->ResolveSubresource(resolve, 0, backBuffer, 0, desc.Format);
						m_deviceCtx->CopyResource(texture, resolve);
						DX_RELEASE(resolve, 0);
					}
				}

				D3D11_MAPPED_SUBRESOURCE mapped;
				DX_CHECK(m_deviceCtx->Map(texture, 0, D3D11_MAP_READ, 0, &mapped) );
				imageSwizzleBgra8(backBufferDesc.Width
					, backBufferDesc.Height
					, mapped.RowPitch
					, mapped.pData
					, mapped.pData
					);
				g_callback->screenShot(_filePath
					, backBufferDesc.Width
					, backBufferDesc.Height
					, mapped.RowPitch
					, mapped.pData
					, backBufferDesc.Height*mapped.RowPitch
					, false
					);
				m_deviceCtx->Unmap(texture, 0);

				DX_RELEASE(texture, 0);
			}

			DX_RELEASE(backBuffer, 0);
		}

		void updateViewName(uint8_t _id, const char* _name) BX_OVERRIDE
		{
			if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
			{
				mbstowcs(&s_viewNameW[_id][BGFX_CONFIG_MAX_VIEW_NAME_RESERVED]
					, _name
					, BX_COUNTOF(s_viewNameW[0])-BGFX_CONFIG_MAX_VIEW_NAME_RESERVED
					);
			}
		}

		void updateUniform(uint16_t _loc, const void* _data, uint32_t _size) BX_OVERRIDE
		{
			memcpy(m_uniforms[_loc], _data, _size);
		}

		void setMarker(const char* _marker, uint32_t _size) BX_OVERRIDE
		{
			if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
			{
				uint32_t size = _size*sizeof(wchar_t);
				wchar_t* name = (wchar_t*)alloca(size);
				mbstowcs(name, _marker, size-2);
				PIX_SETMARKER(D3DCOLOR_RGBA(0xff, 0xff, 0xff, 0xff), name);
			}
		}

		void submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter) BX_OVERRIDE;

		void blitSetup(TextVideoMemBlitter& _blitter) BX_OVERRIDE
		{
			ID3D11DeviceContext* deviceCtx = m_deviceCtx;

			uint32_t width  = getBufferWidth();
			uint32_t height = getBufferHeight();
			if (m_ovr.isEnabled() )
			{
				m_ovr.getSize(width, height);
			}

			FrameBufferHandle fbh = BGFX_INVALID_HANDLE;
			setFrameBuffer(fbh, false);

			D3D11_VIEWPORT vp;
			vp.TopLeftX = 0;
			vp.TopLeftY = 0;
			vp.Width    = (float)width;
			vp.Height   = (float)height;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			deviceCtx->RSSetViewports(1, &vp);

			uint64_t state = BGFX_STATE_RGB_WRITE
				| BGFX_STATE_ALPHA_WRITE
				| BGFX_STATE_DEPTH_TEST_ALWAYS
				;

			setBlendState(state);
			setDepthStencilState(state);
			setRasterizerState(state);

			ProgramD3D11& program = m_program[_blitter.m_program.idx];
			m_currentProgram = &program;
			deviceCtx->VSSetShader(program.m_vsh->m_vertexShader, NULL, 0);
			deviceCtx->VSSetConstantBuffers(0, 1, &program.m_vsh->m_buffer);
			deviceCtx->PSSetShader(program.m_fsh->m_pixelShader, NULL, 0);
			deviceCtx->PSSetConstantBuffers(0, 1, &program.m_fsh->m_buffer);

			VertexBufferD3D11& vb = m_vertexBuffers[_blitter.m_vb->handle.idx];
			VertexDecl& vertexDecl = m_vertexDecls[_blitter.m_vb->decl.idx];
			uint32_t stride = vertexDecl.m_stride;
			uint32_t offset = 0;
			deviceCtx->IASetVertexBuffers(0, 1, &vb.m_ptr, &stride, &offset);
			setInputLayout(vertexDecl, program, 0);

			IndexBufferD3D11& ib = m_indexBuffers[_blitter.m_ib->handle.idx];
			deviceCtx->IASetIndexBuffer(ib.m_ptr, DXGI_FORMAT_R16_UINT, 0);

			float proj[16];
			bx::mtxOrtho(proj, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1000.0f);

			PredefinedUniform& predefined = program.m_predefined[0];
			uint8_t flags = predefined.m_type;
			setShaderUniform(flags, predefined.m_loc, proj, 4);

			commitShaderConstants();
			m_textures[_blitter.m_texture.idx].commit(0);
			commitTextureStage();
		}

		void blitRender(TextVideoMemBlitter& _blitter, uint32_t _numIndices) BX_OVERRIDE
		{
			const uint32_t numVertices = _numIndices*4/6;
			if (0 < numVertices)
			{
				ID3D11DeviceContext* deviceCtx = m_deviceCtx;

				m_indexBuffers [_blitter.m_ib->handle.idx].update(0, _numIndices*2, _blitter.m_ib->data);
				m_vertexBuffers[_blitter.m_vb->handle.idx].update(0, numVertices*_blitter.m_decl.m_stride, _blitter.m_vb->data, true);

				deviceCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				deviceCtx->DrawIndexed(_numIndices, 0, 0);
			}
		}

		void preReset()
		{
			ovrPreReset();

			m_gpuTimer.preReset();

			if (NULL == g_platformData.backBufferDS)
			{
				DX_RELEASE(m_backBufferDepthStencil, 0);
			}

			if (NULL != m_swapChain)
			{
				DX_RELEASE(m_backBufferColor, 0);
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
			{
				m_frameBuffers[ii].preReset();
			}

//			invalidateCache();

			capturePreReset();
		}

		void postReset()
		{
			if (NULL != m_swapChain)
			{
				ID3D11Texture2D* color;
				DX_CHECK(m_swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&color) );

				D3D11_RENDER_TARGET_VIEW_DESC desc;
				desc.ViewDimension = (m_flags & BGFX_RESET_MSAA_MASK)
					? D3D11_RTV_DIMENSION_TEXTURE2DMS
					: D3D11_RTV_DIMENSION_TEXTURE2D
					;
				desc.Texture2D.MipSlice = 0;
				desc.Format = (m_flags & BGFX_RESET_SRGB_BACKBUFFER)
					? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
					: DXGI_FORMAT_R8G8B8A8_UNORM
					;

				DX_CHECK(m_device->CreateRenderTargetView(color, &desc, &m_backBufferColor) );
				DX_RELEASE(color, 0);
			}

			m_gpuTimer.postReset();

			ovrPostReset();

			// If OVR doesn't create separate depth stencil view, create default one.
			if (NULL == m_backBufferDepthStencil)
			{
				D3D11_TEXTURE2D_DESC dsd;
				dsd.Width  = getBufferWidth();
				dsd.Height = getBufferHeight();
				dsd.MipLevels  = 1;
				dsd.ArraySize  = 1;
				dsd.Format     = DXGI_FORMAT_D24_UNORM_S8_UINT;
				dsd.SampleDesc = m_scd.SampleDesc;
				dsd.Usage      = D3D11_USAGE_DEFAULT;
				dsd.BindFlags  = D3D11_BIND_DEPTH_STENCIL;
				dsd.CPUAccessFlags = 0;
				dsd.MiscFlags      = 0;

				ID3D11Texture2D* depthStencil;
				DX_CHECK(m_device->CreateTexture2D(&dsd, NULL, &depthStencil) );
				DX_CHECK(m_device->CreateDepthStencilView(depthStencil, NULL, &m_backBufferDepthStencil) );
				DX_RELEASE(depthStencil, 0);
			}

			m_deviceCtx->OMSetRenderTargets(1, &m_backBufferColor, m_backBufferDepthStencil);

			m_currentColor = m_backBufferColor;
			m_currentDepthStencil = m_backBufferDepthStencil;

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
			{
				m_frameBuffers[ii].postReset();
			}

			capturePostReset();
		}

		static bool isLost(HRESULT _hr)
		{
			return DXGI_ERROR_DEVICE_REMOVED == _hr
				|| DXGI_ERROR_DEVICE_HUNG == _hr
				|| DXGI_ERROR_DEVICE_RESET == _hr
				|| DXGI_ERROR_DRIVER_INTERNAL_ERROR == _hr
				|| DXGI_ERROR_NOT_CURRENTLY_AVAILABLE == _hr
				;
		}

		void flip(HMD& _hmd) BX_OVERRIDE
		{
			if (NULL != m_swapChain)
			{
				HRESULT hr = S_OK;
				uint32_t syncInterval = BX_ENABLED(BX_PLATFORM_WINRT)
					? 1 // sync interval of 0 is not supported on WinRT
					: !!(m_flags & BGFX_RESET_VSYNC)
					;

				for (uint32_t ii = 1, num = m_numWindows; ii < num && SUCCEEDED(hr); ++ii)
				{
					hr = m_frameBuffers[m_windows[ii].idx].m_swapChain->Present(syncInterval, 0);
				}

				if (SUCCEEDED(hr) )
				{
					if (!m_ovr.swap(_hmd) )
					{
						hr = m_swapChain->Present(syncInterval, 0);
					}
				}

				if (FAILED(hr)
				&&  isLost(hr) )
				{
					++m_lost;
					BGFX_FATAL(10 > m_lost, bgfx::Fatal::DeviceLost, "Device is lost. FAILED 0x%08x", hr);
				}
				else
				{
					m_lost = 0;
				}
			}
		}

		void invalidateCache()
		{
			m_inputLayoutCache.invalidate();
			m_blendStateCache.invalidate();
			m_depthStencilStateCache.invalidate();
			m_rasterizerStateCache.invalidate();
			m_samplerStateCache.invalidate();
			m_srvUavLru.invalidate();
		}

		void invalidateCompute()
		{
			m_deviceCtx->CSSetShader(NULL, NULL, 0);

			ID3D11UnorderedAccessView* uav[BGFX_MAX_COMPUTE_BINDINGS] = {};
			m_deviceCtx->CSSetUnorderedAccessViews(0, BX_COUNTOF(uav), uav, NULL);

			ID3D11ShaderResourceView* srv[BGFX_MAX_COMPUTE_BINDINGS] = {};
			m_deviceCtx->CSSetShaderResources(0, BX_COUNTOF(srv), srv);

			ID3D11SamplerState* samplers[BGFX_MAX_COMPUTE_BINDINGS] = {};
			m_deviceCtx->CSSetSamplers(0, BX_COUNTOF(samplers), samplers);
		}

		void updateMsaa()
		{
			for (uint32_t ii = 1, last = 0; ii < BX_COUNTOF(s_msaa); ++ii)
			{
				uint32_t msaa = s_checkMsaa[ii];
				uint32_t quality = 0;
				HRESULT hr = m_device->CheckMultisampleQualityLevels(getBufferFormat(), msaa, &quality);

				if (SUCCEEDED(hr)
				&&  0 < quality)
				{
					s_msaa[ii].Count = msaa;
					s_msaa[ii].Quality = quality - 1;
					last = ii;
				}
				else
				{
					s_msaa[ii] = s_msaa[last];
				}
			}
		}

		void updateResolution(const Resolution& _resolution)
		{
			bool recenter   = !!(_resolution.m_flags & BGFX_RESET_HMD_RECENTER);

			if (!!(_resolution.m_flags & BGFX_RESET_MAXANISOTROPY) )
			{
				m_maxAnisotropy = (m_featureLevel == D3D_FEATURE_LEVEL_9_1)
								? D3D_FL9_1_DEFAULT_MAX_ANISOTROPY
								: D3D11_REQ_MAXANISOTROPY
								;
			}
			else
			{
				m_maxAnisotropy = 1;
			}

			uint32_t flags = _resolution.m_flags & ~(BGFX_RESET_HMD_RECENTER | BGFX_RESET_MAXANISOTROPY);

			if ( getBufferWidth()  != _resolution.m_width
			||   getBufferHeight() != _resolution.m_height
			||   m_flags != flags)
			{
				bool resize = true
					&& !BX_ENABLED(BX_PLATFORM_WINRT) // can't use ResizeBuffers on Windows Phone
					&& (m_flags&BGFX_RESET_MSAA_MASK) == (flags&BGFX_RESET_MSAA_MASK)
					;
				m_flags = flags;

				m_textVideoMem.resize(false, _resolution.m_width, _resolution.m_height);
				m_textVideoMem.clear();

				m_resolution = _resolution;
				m_resolution.m_flags = flags;

				setBufferSize(_resolution.m_width, _resolution.m_height);

				preReset();

				if (NULL == m_swapChain)
				{
					// Updated backbuffer if it changed in PlatformData.
					m_backBufferColor        = (ID3D11RenderTargetView*)g_platformData.backBuffer;
					m_backBufferDepthStencil = (ID3D11DepthStencilView*)g_platformData.backBufferDS;
				}
				else
				{
					if (resize)
					{
						DX_CHECK(m_swapChain->ResizeBuffers(2
							, getBufferWidth()
							, getBufferHeight()
							, getBufferFormat()
							, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
							) );
					}
					else
					{
						updateMsaa();
						m_scd.SampleDesc = s_msaa[(m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT];

						DX_RELEASE(m_swapChain, 0);

						SwapChainDesc* scd = &m_scd;
						SwapChainDesc swapChainScd;
						if (0 != (m_flags & BGFX_RESET_HMD)
						&&  m_ovr.isInitialized() )
						{
							swapChainScd = m_scd;
							swapChainScd.SampleDesc = s_msaa[0];
							scd = &swapChainScd;
						}

#if BX_PLATFORM_WINRT
						HRESULT hr;
						hr = m_factory->CreateSwapChainForCoreWindow(m_device
							, (::IUnknown*)g_platformData.nwh
							, scd
							, NULL
							, &m_swapChain
							);
#else
						HRESULT hr;
						hr = m_factory->CreateSwapChain(m_device
							, scd
							, &m_swapChain
							);
#endif // BX_PLATFORM_WINRT
						BGFX_FATAL(SUCCEEDED(hr), bgfx::Fatal::UnableToInitialize, "Failed to create swap chain.");
					}
				}

				postReset();
			}

			if (recenter)
			{
				m_ovr.recenter();
			}
		}

		void setShaderUniform(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			if (_flags&BGFX_UNIFORM_FRAGMENTBIT)
			{
				memcpy(&m_fsScratch[_regIndex], _val, _numRegs*16);
				m_fsChanges += _numRegs;
			}
			else
			{
				memcpy(&m_vsScratch[_regIndex], _val, _numRegs*16);
				m_vsChanges += _numRegs;
			}
		}

		void setShaderUniform4f(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			setShaderUniform(_flags, _regIndex, _val, _numRegs);
		}

		void setShaderUniform4x4f(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			setShaderUniform(_flags, _regIndex, _val, _numRegs);
		}

		void commitShaderConstants()
		{
			if (0 < m_vsChanges)
			{
				if (NULL != m_currentProgram->m_vsh->m_buffer)
				{
					m_deviceCtx->UpdateSubresource(m_currentProgram->m_vsh->m_buffer, 0, 0, m_vsScratch, 0, 0);
				}

				m_vsChanges = 0;
			}

			if (0 < m_fsChanges)
			{
				if (NULL != m_currentProgram->m_fsh->m_buffer)
				{
					m_deviceCtx->UpdateSubresource(m_currentProgram->m_fsh->m_buffer, 0, 0, m_fsScratch, 0, 0);
				}

				m_fsChanges = 0;
			}
		}

		void setFrameBuffer(FrameBufferHandle _fbh, bool _msaa = true)
		{
			if (isValid(m_fbh)
			&&  m_fbh.idx != _fbh.idx
			&&  m_rtMsaa)
			{
				FrameBufferD3D11& frameBuffer = m_frameBuffers[m_fbh.idx];
				frameBuffer.resolve();
			}

			if (!isValid(_fbh) )
			{
				m_deviceCtx->OMSetRenderTargets(1, &m_backBufferColor, m_backBufferDepthStencil);

				m_currentColor = m_backBufferColor;
				m_currentDepthStencil = m_backBufferDepthStencil;
			}
			else
			{
				invalidateTextureStage();

				FrameBufferD3D11& frameBuffer = m_frameBuffers[_fbh.idx];
				m_deviceCtx->OMSetRenderTargets(frameBuffer.m_num, frameBuffer.m_rtv, frameBuffer.m_dsv);

				m_currentColor = frameBuffer.m_rtv[0];
				m_currentDepthStencil = frameBuffer.m_dsv;
			}

			m_fbh = _fbh;
			m_rtMsaa = _msaa;
		}

		void clear(const Clear& _clear, const float _palette[][4])
		{
			if (isValid(m_fbh) )
			{
				FrameBufferD3D11& frameBuffer = m_frameBuffers[m_fbh.idx];
				frameBuffer.clear(_clear, _palette);
			}
			else
			{
				if (NULL != m_currentColor
				&&  BGFX_CLEAR_COLOR & _clear.m_flags)
				{
					if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
					{
						uint8_t index = _clear.m_index[0];
						if (UINT8_MAX != index)
						{
							m_deviceCtx->ClearRenderTargetView(m_currentColor, _palette[index]);
						}
					}
					else
					{
						float frgba[4] =
						{
							_clear.m_index[0]*1.0f/255.0f,
							_clear.m_index[1]*1.0f/255.0f,
							_clear.m_index[2]*1.0f/255.0f,
							_clear.m_index[3]*1.0f/255.0f,
						};
						m_deviceCtx->ClearRenderTargetView(m_currentColor, frgba);
					}
				}

				if (NULL != m_currentDepthStencil
				&& (BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL) & _clear.m_flags)
				{
					DWORD flags = 0;
					flags |= (_clear.m_flags & BGFX_CLEAR_DEPTH) ? D3D11_CLEAR_DEPTH : 0;
					flags |= (_clear.m_flags & BGFX_CLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0;
					m_deviceCtx->ClearDepthStencilView(m_currentDepthStencil, flags, _clear.m_depth, _clear.m_stencil);
				}
			}
		}

		void setInputLayout(const VertexDecl& _vertexDecl, const ProgramD3D11& _program, uint16_t _numInstanceData)
		{
			uint64_t layoutHash = (uint64_t(_vertexDecl.m_hash)<<32) | _program.m_vsh->m_hash;
			layoutHash ^= _numInstanceData;
			ID3D11InputLayout* layout = m_inputLayoutCache.find(layoutHash);
			if (NULL == layout)
			{
				D3D11_INPUT_ELEMENT_DESC vertexElements[Attrib::Count+1+BGFX_CONFIG_MAX_INSTANCE_DATA_COUNT];

				VertexDecl decl;
				memcpy(&decl, &_vertexDecl, sizeof(VertexDecl) );
				const uint16_t* attrMask = _program.m_vsh->m_attrMask;

				for (uint32_t ii = 0; ii < Attrib::Count; ++ii)
				{
					uint16_t mask = attrMask[ii];
					uint16_t attr = (decl.m_attributes[ii] & mask);
					decl.m_attributes[ii] = attr == 0 ? UINT16_MAX : attr == UINT16_MAX ? 0 : attr;
				}

				D3D11_INPUT_ELEMENT_DESC* elem = fillVertexDecl(vertexElements, decl);
				uint32_t num = uint32_t(elem-vertexElements);

				const D3D11_INPUT_ELEMENT_DESC inst = { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 };

				for (uint32_t ii = 0; ii < _numInstanceData; ++ii)
				{
					uint32_t index = 7-ii; // TEXCOORD7 = i_data0, TEXCOORD6 = i_data1, etc.

					uint32_t jj;
					D3D11_INPUT_ELEMENT_DESC* curr = vertexElements;
					for (jj = 0; jj < num; ++jj)
					{
						curr = &vertexElements[jj];
						if (0 == strcmp(curr->SemanticName, "TEXCOORD")
						&&  curr->SemanticIndex == index)
						{
							break;
						}
					}

					if (jj == num)
					{
						curr = elem;
						++elem;
					}

					memcpy(curr, &inst, sizeof(D3D11_INPUT_ELEMENT_DESC) );
					curr->InputSlot = 1;
					curr->SemanticIndex = index;
					curr->AlignedByteOffset = ii*16;
				}

				num = uint32_t(elem-vertexElements);
				DX_CHECK(m_device->CreateInputLayout(vertexElements
					, num
					, _program.m_vsh->m_code->data
					, _program.m_vsh->m_code->size
					, &layout
					) );
				m_inputLayoutCache.add(layoutHash, layout);
			}

			m_deviceCtx->IASetInputLayout(layout);
		}

		void setBlendState(uint64_t _state, uint32_t _rgba = 0)
		{
			_state &= BGFX_D3D11_BLEND_STATE_MASK;

			bx::HashMurmur2A murmur;
			murmur.begin();
			murmur.add(_state);

			const uint64_t f0 = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_FACTOR);
			const uint64_t f1 = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_INV_FACTOR, BGFX_STATE_BLEND_INV_FACTOR);
			bool hasFactor = f0 == (_state & f0)
				|| f1 == (_state & f1)
				;

			float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (hasFactor)
			{
				blendFactor[0] = ( (_rgba>>24)     )/255.0f;
				blendFactor[1] = ( (_rgba>>16)&0xff)/255.0f;
				blendFactor[2] = ( (_rgba>> 8)&0xff)/255.0f;
				blendFactor[3] = ( (_rgba    )&0xff)/255.0f;
			}
			else
			{
				murmur.add(_rgba);
			}

			uint32_t hash = murmur.end();

			ID3D11BlendState* bs = m_blendStateCache.find(hash);
			if (NULL == bs)
			{
				D3D11_BLEND_DESC desc;
				memset(&desc, 0, sizeof(desc) );
				desc.IndependentBlendEnable = !!(BGFX_STATE_BLEND_INDEPENDENT & _state);

				D3D11_RENDER_TARGET_BLEND_DESC* drt = &desc.RenderTarget[0];
				drt->BlendEnable = !!(BGFX_STATE_BLEND_MASK & _state);

				const uint32_t blend    = uint32_t( (_state&BGFX_STATE_BLEND_MASK)>>BGFX_STATE_BLEND_SHIFT);
				const uint32_t equation = uint32_t( (_state&BGFX_STATE_BLEND_EQUATION_MASK)>>BGFX_STATE_BLEND_EQUATION_SHIFT);

				const uint32_t srcRGB = (blend      ) & 0xf;
				const uint32_t dstRGB = (blend >>  4) & 0xf;
				const uint32_t srcA   = (blend >>  8) & 0xf;
				const uint32_t dstA   = (blend >> 12) & 0xf;

				const uint32_t equRGB = (equation     ) & 0x7;
				const uint32_t equA   = (equation >> 3) & 0x7;

				drt->SrcBlend       = s_blendFactor[srcRGB][0];
				drt->DestBlend      = s_blendFactor[dstRGB][0];
				drt->BlendOp        = s_blendEquation[equRGB];

				drt->SrcBlendAlpha  = s_blendFactor[srcA][1];
				drt->DestBlendAlpha = s_blendFactor[dstA][1];
				drt->BlendOpAlpha   = s_blendEquation[equA];

				uint8_t writeMask = (_state&BGFX_STATE_ALPHA_WRITE)
					? D3D11_COLOR_WRITE_ENABLE_ALPHA
					: 0
					;
				writeMask |= (_state&BGFX_STATE_RGB_WRITE)
					? D3D11_COLOR_WRITE_ENABLE_RED
					| D3D11_COLOR_WRITE_ENABLE_GREEN
					| D3D11_COLOR_WRITE_ENABLE_BLUE
					: 0
					;

				drt->RenderTargetWriteMask = writeMask;

				if (desc.IndependentBlendEnable)
				{
					for (uint32_t ii = 1, rgba = _rgba; ii < BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS; ++ii, rgba >>= 11)
					{
						drt = &desc.RenderTarget[ii];
						drt->BlendEnable = 0 != (rgba & 0x7ff);

						const uint32_t src = (rgba     ) & 0xf;
						const uint32_t dst = (rgba >> 4) & 0xf;
						const uint32_t equ = (rgba >> 8) & 0x7;

						drt->SrcBlend       = s_blendFactor[src][0];
						drt->DestBlend      = s_blendFactor[dst][0];
						drt->BlendOp        = s_blendEquation[equ];

						drt->SrcBlendAlpha  = s_blendFactor[src][1];
						drt->DestBlendAlpha = s_blendFactor[dst][1];
						drt->BlendOpAlpha   = s_blendEquation[equ];

						drt->RenderTargetWriteMask = writeMask;
					}
				}
				else
				{
					for (uint32_t ii = 1; ii < BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS; ++ii)
					{
						memcpy(&desc.RenderTarget[ii], drt, sizeof(D3D11_RENDER_TARGET_BLEND_DESC) );
					}
				}

				DX_CHECK(m_device->CreateBlendState(&desc, &bs) );

				m_blendStateCache.add(hash, bs);
			}

			m_deviceCtx->OMSetBlendState(bs, blendFactor, 0xffffffff);
		}

		void setDepthStencilState(uint64_t _state, uint64_t _stencil = 0)
		{
			_state &= BGFX_D3D11_DEPTH_STENCIL_MASK;

			uint32_t fstencil = unpackStencil(0, _stencil);
			uint32_t ref = (fstencil&BGFX_STENCIL_FUNC_REF_MASK)>>BGFX_STENCIL_FUNC_REF_SHIFT;
			_stencil &= packStencil(~BGFX_STENCIL_FUNC_REF_MASK, BGFX_STENCIL_MASK);

			bx::HashMurmur2A murmur;
			murmur.begin();
			murmur.add(_state);
			murmur.add(_stencil);
			uint32_t hash = murmur.end();

			ID3D11DepthStencilState* dss = m_depthStencilStateCache.find(hash);
			if (NULL == dss)
			{
				D3D11_DEPTH_STENCIL_DESC desc;
				memset(&desc, 0, sizeof(desc) );
				uint32_t func = (_state&BGFX_STATE_DEPTH_TEST_MASK)>>BGFX_STATE_DEPTH_TEST_SHIFT;
				desc.DepthEnable = 0 != func;
				desc.DepthWriteMask = !!(BGFX_STATE_DEPTH_WRITE & _state) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
				desc.DepthFunc = s_cmpFunc[func];

				uint32_t bstencil = unpackStencil(1, _stencil);
				uint32_t frontAndBack = bstencil != BGFX_STENCIL_NONE && bstencil != fstencil;
				bstencil = frontAndBack ? bstencil : fstencil;

				desc.StencilEnable = 0 != _stencil;
				desc.StencilReadMask = (fstencil&BGFX_STENCIL_FUNC_RMASK_MASK)>>BGFX_STENCIL_FUNC_RMASK_SHIFT;
				desc.StencilWriteMask = 0xff;
				desc.FrontFace.StencilFailOp      = s_stencilOp[(fstencil&BGFX_STENCIL_OP_FAIL_S_MASK)>>BGFX_STENCIL_OP_FAIL_S_SHIFT];
				desc.FrontFace.StencilDepthFailOp = s_stencilOp[(fstencil&BGFX_STENCIL_OP_FAIL_Z_MASK)>>BGFX_STENCIL_OP_FAIL_Z_SHIFT];
				desc.FrontFace.StencilPassOp      = s_stencilOp[(fstencil&BGFX_STENCIL_OP_PASS_Z_MASK)>>BGFX_STENCIL_OP_PASS_Z_SHIFT];
				desc.FrontFace.StencilFunc        = s_cmpFunc[(fstencil&BGFX_STENCIL_TEST_MASK)>>BGFX_STENCIL_TEST_SHIFT];
				desc.BackFace.StencilFailOp       = s_stencilOp[(bstencil&BGFX_STENCIL_OP_FAIL_S_MASK)>>BGFX_STENCIL_OP_FAIL_S_SHIFT];
				desc.BackFace.StencilDepthFailOp  = s_stencilOp[(bstencil&BGFX_STENCIL_OP_FAIL_Z_MASK)>>BGFX_STENCIL_OP_FAIL_Z_SHIFT];
				desc.BackFace.StencilPassOp       = s_stencilOp[(bstencil&BGFX_STENCIL_OP_PASS_Z_MASK)>>BGFX_STENCIL_OP_PASS_Z_SHIFT];
				desc.BackFace.StencilFunc         = s_cmpFunc[(bstencil&BGFX_STENCIL_TEST_MASK)>>BGFX_STENCIL_TEST_SHIFT];

				DX_CHECK(m_device->CreateDepthStencilState(&desc, &dss) );

				m_depthStencilStateCache.add(hash, dss);
			}

			m_deviceCtx->OMSetDepthStencilState(dss, ref);
		}

		void setDebugWireframe(bool _wireframe)
		{
			if (m_wireframe != _wireframe)
			{
				m_wireframe = _wireframe;
				m_rasterizerStateCache.invalidate();
			}
		}

		void setRasterizerState(uint64_t _state, bool _wireframe = false, bool _scissor = false)
		{
			_state &= BGFX_STATE_CULL_MASK|BGFX_STATE_MSAA;
			_state |= _wireframe ? BGFX_STATE_PT_LINES : BGFX_STATE_NONE;
			_state |= _scissor ? BGFX_STATE_RESERVED_MASK : 0;

			ID3D11RasterizerState* rs = m_rasterizerStateCache.find(_state);
			if (NULL == rs)
			{
				uint32_t cull = (_state&BGFX_STATE_CULL_MASK)>>BGFX_STATE_CULL_SHIFT;

				D3D11_RASTERIZER_DESC desc;
				desc.FillMode = _wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
				desc.CullMode = s_cullMode[cull];
				desc.FrontCounterClockwise = false;
				desc.DepthBias = 0;
				desc.DepthBiasClamp = 0.0f;
				desc.SlopeScaledDepthBias = 0.0f;
				desc.DepthClipEnable = m_featureLevel <= D3D_FEATURE_LEVEL_9_3;	// disabling depth clip is only supported on 10_0+
				desc.ScissorEnable = _scissor;
				desc.MultisampleEnable = !!(_state&BGFX_STATE_MSAA);
				desc.AntialiasedLineEnable = false;

				DX_CHECK(m_device->CreateRasterizerState(&desc, &rs) );

				m_rasterizerStateCache.add(_state, rs);
			}

			m_deviceCtx->RSSetState(rs);
		}

		ID3D11SamplerState* getSamplerState(uint32_t _flags)
		{
			_flags &= BGFX_TEXTURE_SAMPLER_BITS_MASK;
			ID3D11SamplerState* sampler = m_samplerStateCache.find(_flags);
			if (NULL == sampler)
			{
				const uint32_t cmpFunc = (_flags&BGFX_TEXTURE_COMPARE_MASK)>>BGFX_TEXTURE_COMPARE_SHIFT;
				const uint8_t minFilter = s_textureFilter[0][(_flags&BGFX_TEXTURE_MIN_MASK)>>BGFX_TEXTURE_MIN_SHIFT];
				const uint8_t magFilter = s_textureFilter[1][(_flags&BGFX_TEXTURE_MAG_MASK)>>BGFX_TEXTURE_MAG_SHIFT];
				const uint8_t mipFilter = s_textureFilter[2][(_flags&BGFX_TEXTURE_MIP_MASK)>>BGFX_TEXTURE_MIP_SHIFT];
				const uint8_t filter = 0 == cmpFunc ? 0 : D3D11_COMPARISON_FILTERING_BIT;

				D3D11_SAMPLER_DESC sd;
				sd.Filter = (D3D11_FILTER)(filter|minFilter|magFilter|mipFilter);
				sd.AddressU = s_textureAddress[(_flags&BGFX_TEXTURE_U_MASK)>>BGFX_TEXTURE_U_SHIFT];
				sd.AddressV = s_textureAddress[(_flags&BGFX_TEXTURE_V_MASK)>>BGFX_TEXTURE_V_SHIFT];
				sd.AddressW = s_textureAddress[(_flags&BGFX_TEXTURE_W_MASK)>>BGFX_TEXTURE_W_SHIFT];
				sd.MipLODBias = 0.0f;
				sd.MaxAnisotropy  = m_maxAnisotropy;
				sd.ComparisonFunc = 0 == cmpFunc ? D3D11_COMPARISON_NEVER : s_cmpFunc[cmpFunc];
				sd.BorderColor[0] = 0.0f;
				sd.BorderColor[1] = 0.0f;
				sd.BorderColor[2] = 0.0f;
				sd.BorderColor[3] = 0.0f;
				sd.MinLOD = 0;
				sd.MaxLOD = D3D11_FLOAT32_MAX;

				m_device->CreateSamplerState(&sd, &sampler);
				DX_CHECK_REFCOUNT(sampler, 1);

				m_samplerStateCache.add(_flags, sampler);
			}

			return sampler;
		}

		DXGI_FORMAT getBufferFormat()
		{
#if BX_PLATFORM_WINRT
			return m_scd.Format;
#else
			return m_scd.BufferDesc.Format;
#endif
		}

		uint32_t getBufferWidth()
		{
#if BX_PLATFORM_WINRT
			return m_scd.Width;
#else
			return m_scd.BufferDesc.Width;
#endif
		}

		uint32_t getBufferHeight()
		{
#if BX_PLATFORM_WINRT
			return m_scd.Height;
#else
			return m_scd.BufferDesc.Height;
#endif
		}

		void setBufferSize(uint32_t _width, uint32_t _height)
		{
#if BX_PLATFORM_WINRT
			m_scd.Width  = _width;
			m_scd.Height = _height;
#else
			m_scd.BufferDesc.Width  = _width;
			m_scd.BufferDesc.Height = _height;
#endif
		}

		void commitTextureStage()
		{
			// vertex texture fetch not supported on 9_1 through 9_3
			if (m_featureLevel > D3D_FEATURE_LEVEL_9_3)
			{
				m_deviceCtx->VSSetShaderResources(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, m_textureStage.m_srv);
				m_deviceCtx->VSSetSamplers(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, m_textureStage.m_sampler);
			}

			m_deviceCtx->PSSetShaderResources(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, m_textureStage.m_srv);
			m_deviceCtx->PSSetSamplers(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, m_textureStage.m_sampler);
		}

		void invalidateTextureStage()
		{
			m_textureStage.clear();
			commitTextureStage();
		}

		ID3D11UnorderedAccessView* getCachedUav(TextureHandle _handle, uint8_t _mip)
		{
			bx::HashMurmur2A murmur;
			murmur.begin();
			murmur.add(_handle);
			murmur.add(_mip);
			murmur.add(0);
			uint32_t hash = murmur.end();

			IUnknown** ptr = m_srvUavLru.find(hash);
			ID3D11UnorderedAccessView* uav;
			if (NULL == ptr)
			{
				TextureD3D11& texture = m_textures[_handle.idx];

				D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
				desc.Format = s_textureFormat[texture.m_textureFormat].m_fmtSrv;
				switch (texture.m_type)
				{
				case TextureD3D11::Texture2D:
				case TextureD3D11::TextureCube:
					desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipSlice = _mip;
					break;

				case TextureD3D11::Texture3D:
					desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
					desc.Texture3D.MipSlice    = _mip;
					desc.Texture3D.FirstWSlice = 0;
					desc.Texture3D.WSize       = UINT32_MAX;
					break;
				}

				DX_CHECK(m_device->CreateUnorderedAccessView(texture.m_ptr, &desc, &uav) );

				m_srvUavLru.add(hash, uav, _handle.idx);
			}
			else
			{
				uav = static_cast<ID3D11UnorderedAccessView*>(*ptr);
			}

			return uav;
		}

		ID3D11ShaderResourceView* getCachedSrv(TextureHandle _handle, uint8_t _mip)
		{
			bx::HashMurmur2A murmur;
			murmur.begin();
			murmur.add(_handle);
			murmur.add(_mip);
			murmur.add(0);
			uint32_t hash = murmur.end();

			IUnknown** ptr = m_srvUavLru.find(hash);
			ID3D11ShaderResourceView* srv;
			if (NULL == ptr)
			{
				TextureD3D11& texture = m_textures[_handle.idx];

				D3D11_SHADER_RESOURCE_VIEW_DESC desc;
				desc.Format = s_textureFormat[texture.m_textureFormat].m_fmtSrv;
				switch (texture.m_type)
				{
				case TextureD3D11::Texture2D:
					desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MostDetailedMip = _mip;
					desc.Texture2D.MipLevels       = 1;
					break;

				case TextureD3D11::TextureCube:
					desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
					desc.TextureCube.MostDetailedMip = _mip;
					desc.TextureCube.MipLevels       = 1;
					break;

				case TextureD3D11::Texture3D:
					desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
					desc.Texture3D.MostDetailedMip = _mip;
					desc.Texture3D.MipLevels       = 1;
					break;
				}

				DX_CHECK(m_device->CreateShaderResourceView(texture.m_ptr, &desc, &srv) );

				m_srvUavLru.add(hash, srv, _handle.idx);
			}
			else
			{
				srv = static_cast<ID3D11ShaderResourceView*>(*ptr);
			}

			return srv;
		}

		void ovrPostReset()
		{
#if BGFX_CONFIG_USE_OVR
			if (m_flags & (BGFX_RESET_HMD|BGFX_RESET_HMD_DEBUG) )
			{
				ovrD3D11Config config;
				config.D3D11.Header.API = ovrRenderAPI_D3D11;
#	if OVR_VERSION > OVR_VERSION_043
				config.D3D11.Header.BackBufferSize.w = m_scd.BufferDesc.Width;
				config.D3D11.Header.BackBufferSize.h = m_scd.BufferDesc.Height;
				config.D3D11.pBackBufferUAV = NULL;
#	else
				config.D3D11.Header.RTSize.w = m_scd.BufferDesc.Width;
				config.D3D11.Header.RTSize.h = m_scd.BufferDesc.Height;
#	endif // OVR_VERSION > OVR_VERSION_042
				config.D3D11.Header.Multisample = 0;
				config.D3D11.pDevice        = m_device;
				config.D3D11.pDeviceContext = m_deviceCtx;
				config.D3D11.pBackBufferRT  = m_backBufferColor;
				config.D3D11.pSwapChain     = m_swapChain;
				if (m_ovr.postReset(g_platformData.nwh, &config.Config, !!(m_flags & BGFX_RESET_HMD_DEBUG) ) )
				{
					uint32_t size = sizeof(uint32_t) + sizeof(TextureCreate);
					const Memory* mem = alloc(size);

					bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
					uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
					bx::write(&writer, magic);

					TextureCreate tc;
					tc.m_flags   = BGFX_TEXTURE_RT|( ((m_flags & BGFX_RESET_MSAA_MASK) >> BGFX_RESET_MSAA_SHIFT) << BGFX_TEXTURE_RT_MSAA_SHIFT);
					tc.m_width   = m_ovr.m_rtSize.w;
					tc.m_height  = m_ovr.m_rtSize.h;
					tc.m_sides   = 0;
					tc.m_depth   = 0;
					tc.m_numMips = 1;
					tc.m_format  = uint8_t(bgfx::TextureFormat::BGRA8);
					tc.m_cubeMap = false;
					tc.m_mem     = NULL;
					bx::write(&writer, tc);
					m_ovrRT.create(mem, tc.m_flags, 0);

					release(mem);

					DX_CHECK(m_device->CreateRenderTargetView(m_ovrRT.m_ptr, NULL, &m_ovrRtv) );

					D3D11_TEXTURE2D_DESC dsd;
					dsd.Width      = m_ovr.m_rtSize.w;
					dsd.Height     = m_ovr.m_rtSize.h;
					dsd.MipLevels  = 1;
					dsd.ArraySize  = 1;
					dsd.Format     = DXGI_FORMAT_D24_UNORM_S8_UINT;
					dsd.SampleDesc = m_scd.SampleDesc;
					dsd.Usage      = D3D11_USAGE_DEFAULT;
					dsd.BindFlags  = D3D11_BIND_DEPTH_STENCIL;
					dsd.CPUAccessFlags = 0;
					dsd.MiscFlags      = 0;

					ID3D11Texture2D* depthStencil;
					DX_CHECK(m_device->CreateTexture2D(&dsd, NULL, &depthStencil) );
					DX_CHECK(m_device->CreateDepthStencilView(depthStencil, NULL, &m_ovrDsv) );
					DX_RELEASE(depthStencil, 0);

					ovrD3D11Texture texture;
					texture.D3D11.Header.API         = ovrRenderAPI_D3D11;
					texture.D3D11.Header.TextureSize = m_ovr.m_rtSize;
					texture.D3D11.pTexture           = m_ovrRT.m_texture2d;
					texture.D3D11.pSRView            = m_ovrRT.m_srv;
					m_ovr.postReset(texture.Texture);

					bx::xchg(m_ovrRtv, m_backBufferColor);

					BX_CHECK(NULL == m_backBufferDepthStencil, "");
					bx::xchg(m_ovrDsv, m_backBufferDepthStencil);
				}
			}
#endif // BGFX_CONFIG_USE_OVR
		}

		void ovrPreReset()
		{
#if BGFX_CONFIG_USE_OVR
			m_ovr.preReset();
			if (NULL != m_ovrRtv)
			{
				bx::xchg(m_ovrRtv, m_backBufferColor);
				bx::xchg(m_ovrDsv, m_backBufferDepthStencil);
				BX_CHECK(NULL == m_backBufferDepthStencil, "");

				DX_RELEASE(m_ovrRtv, 0);
				DX_RELEASE(m_ovrDsv, 0);
				m_ovrRT.destroy();
			}
#endif // BGFX_CONFIG_USE_OVR
		}

		void capturePostReset()
		{
			if (m_flags&BGFX_RESET_CAPTURE)
			{
				ID3D11Texture2D* backBuffer;
				DX_CHECK(m_swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&backBuffer) );

				D3D11_TEXTURE2D_DESC backBufferDesc;
				backBuffer->GetDesc(&backBufferDesc);

				D3D11_TEXTURE2D_DESC desc;
				memcpy(&desc, &backBufferDesc, sizeof(desc) );
				desc.SampleDesc.Count   = 1;
				desc.SampleDesc.Quality = 0;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.BindFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

				HRESULT hr = m_device->CreateTexture2D(&desc, NULL, &m_captureTexture);
				if (SUCCEEDED(hr) )
				{
					if (backBufferDesc.SampleDesc.Count != 1)
					{
						desc.Usage = D3D11_USAGE_DEFAULT;
						desc.CPUAccessFlags = 0;
						m_device->CreateTexture2D(&desc, NULL, &m_captureResolve);
					}

					g_callback->captureBegin(backBufferDesc.Width, backBufferDesc.Height, backBufferDesc.Width*4, TextureFormat::BGRA8, false);
				}

				DX_RELEASE(backBuffer, 0);
			}
		}

		void capturePreReset()
		{
			if (NULL != m_captureTexture)
			{
				g_callback->captureEnd();
			}

			DX_RELEASE(m_captureResolve, 0);
			DX_RELEASE(m_captureTexture, 0);
		}

		void capture()
		{
			if (NULL != m_captureTexture)
			{
				ID3D11Texture2D* backBuffer;
				DX_CHECK(m_swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&backBuffer) );

				if (NULL == m_captureResolve)
				{
					m_deviceCtx->CopyResource(m_captureTexture, backBuffer);
				}
				else
				{
					m_deviceCtx->ResolveSubresource(m_captureResolve, 0, backBuffer, 0, getBufferFormat() );
					m_deviceCtx->CopyResource(m_captureTexture, m_captureResolve);
				}

				D3D11_MAPPED_SUBRESOURCE mapped;
				DX_CHECK(m_deviceCtx->Map(m_captureTexture, 0, D3D11_MAP_READ, 0, &mapped) );

				g_callback->captureFrame(mapped.pData, getBufferHeight()*mapped.RowPitch);

				m_deviceCtx->Unmap(m_captureTexture, 0);

				DX_RELEASE(backBuffer, 0);
			}
		}

		void commit(ConstantBuffer& _constantBuffer)
		{
			_constantBuffer.reset();

			for (;;)
			{
				uint32_t opcode = _constantBuffer.read();

				if (UniformType::End == opcode)
				{
					break;
				}

				UniformType::Enum type;
				uint16_t loc;
				uint16_t num;
				uint16_t copy;
				ConstantBuffer::decodeOpcode(opcode, type, loc, num, copy);

				const char* data;
				if (copy)
				{
					data = _constantBuffer.read(g_uniformTypeSize[type]*num);
				}
				else
				{
					UniformHandle handle;
					memcpy(&handle, _constantBuffer.read(sizeof(UniformHandle) ), sizeof(UniformHandle) );
					data = (const char*)m_uniforms[handle.idx];
				}

#define CASE_IMPLEMENT_UNIFORM(_uniform, _dxsuffix, _type) \
		case UniformType::_uniform: \
		case UniformType::_uniform|BGFX_UNIFORM_FRAGMENTBIT: \
				{ \
					setShaderUniform(uint8_t(type), loc, data, num); \
				} \
				break;

				switch ( (uint32_t)type)
				{
				case UniformType::Mat3:
				case UniformType::Mat3|BGFX_UNIFORM_FRAGMENTBIT: \
					 {
						 float* value = (float*)data;
						 for (uint32_t ii = 0, count = num/3; ii < count; ++ii,  loc += 3*16, value += 9)
						 {
							 Matrix4 mtx;
							 mtx.un.val[ 0] = value[0];
							 mtx.un.val[ 1] = value[1];
							 mtx.un.val[ 2] = value[2];
							 mtx.un.val[ 3] = 0.0f;
							 mtx.un.val[ 4] = value[3];
							 mtx.un.val[ 5] = value[4];
							 mtx.un.val[ 6] = value[5];
							 mtx.un.val[ 7] = 0.0f;
							 mtx.un.val[ 8] = value[6];
							 mtx.un.val[ 9] = value[7];
							 mtx.un.val[10] = value[8];
							 mtx.un.val[11] = 0.0f;
							 setShaderUniform(uint8_t(type), loc, &mtx.un.val[0], 3);
						 }
					}
					break;

				CASE_IMPLEMENT_UNIFORM(Int1, I, int);
				CASE_IMPLEMENT_UNIFORM(Vec4, F, float);
				CASE_IMPLEMENT_UNIFORM(Mat4, F, float);

				case UniformType::End:
					break;

				default:
					BX_TRACE("%4d: INVALID 0x%08x, t %d, l %d, n %d, c %d", _constantBuffer.getPos(), opcode, type, loc, num, copy);
					break;
				}
#undef CASE_IMPLEMENT_UNIFORM
			}
		}

		void clearQuad(ClearQuad& _clearQuad, const Rect& _rect, const Clear& _clear, const float _palette[][4])
		{
			uint32_t width  = getBufferWidth();
			uint32_t height = getBufferHeight();

			if (0      == _rect.m_x
			&&  0      == _rect.m_y
			&&  width  == _rect.m_width
			&&  height == _rect.m_height)
			{
				clear(_clear, _palette);
			}
			else
			{
				ID3D11DeviceContext* deviceCtx = m_deviceCtx;

				uint64_t state = 0;
				state |= _clear.m_flags & BGFX_CLEAR_COLOR ? BGFX_STATE_RGB_WRITE|BGFX_STATE_ALPHA_WRITE : 0;
				state |= _clear.m_flags & BGFX_CLEAR_DEPTH ? BGFX_STATE_DEPTH_TEST_ALWAYS|BGFX_STATE_DEPTH_WRITE : 0;

				uint64_t stencil = 0;
				stencil |= _clear.m_flags & BGFX_CLEAR_STENCIL ? 0
					| BGFX_STENCIL_TEST_ALWAYS
					| BGFX_STENCIL_FUNC_REF(_clear.m_stencil)
					| BGFX_STENCIL_FUNC_RMASK(0xff)
					| BGFX_STENCIL_OP_FAIL_S_REPLACE
					| BGFX_STENCIL_OP_FAIL_Z_REPLACE
					| BGFX_STENCIL_OP_PASS_Z_REPLACE
					: 0
					;

				setBlendState(state);
				setDepthStencilState(state, stencil);
				setRasterizerState(state);

				uint32_t numMrt = 1;
				FrameBufferHandle fbh = m_fbh;
				if (isValid(fbh) )
				{
					const FrameBufferD3D11& fb = m_frameBuffers[fbh.idx];
					numMrt = bx::uint32_max(1, fb.m_num);
				}

				ProgramD3D11& program = m_program[_clearQuad.m_program[numMrt-1].idx];
				m_currentProgram = &program;
				deviceCtx->VSSetShader(program.m_vsh->m_vertexShader, NULL, 0);
				deviceCtx->VSSetConstantBuffers(0, 1, s_zero.m_buffer);
				if (NULL != m_currentColor)
				{
					const ShaderD3D11* fsh = program.m_fsh;
					deviceCtx->PSSetShader(fsh->m_pixelShader, NULL, 0);
					deviceCtx->PSSetConstantBuffers(0, 1, &fsh->m_buffer);

					if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
					{
						float mrtClear[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS][4];
						for (uint32_t ii = 0; ii < numMrt; ++ii)
						{
							uint8_t index = (uint8_t)bx::uint32_min(BGFX_CONFIG_MAX_CLEAR_COLOR_PALETTE-1, _clear.m_index[ii]);
							memcpy(mrtClear[ii], _palette[index], 16);
						}

						deviceCtx->UpdateSubresource(fsh->m_buffer, 0, 0, mrtClear, 0, 0);
					}
					else
					{
						float rgba[4] =
						{
							_clear.m_index[0]*1.0f/255.0f,
							_clear.m_index[1]*1.0f/255.0f,
							_clear.m_index[2]*1.0f/255.0f,
							_clear.m_index[3]*1.0f/255.0f,
						};

						deviceCtx->UpdateSubresource(fsh->m_buffer, 0, 0, rgba, 0, 0);
					}
				}
				else
				{
					deviceCtx->PSSetShader(NULL, NULL, 0);
				}

				VertexBufferD3D11& vb = m_vertexBuffers[_clearQuad.m_vb->handle.idx];
				const VertexDecl& vertexDecl = m_vertexDecls[_clearQuad.m_vb->decl.idx];
				const uint32_t stride = vertexDecl.m_stride;
				const uint32_t offset = 0;

				{
					struct Vertex
					{
						float m_x;
						float m_y;
						float m_z;
					};

					Vertex* vertex = (Vertex*)_clearQuad.m_vb->data;
					BX_CHECK(stride == sizeof(Vertex), "Stride/Vertex mismatch (stride %d, sizeof(Vertex) %d)", stride, sizeof(Vertex) );

					const float depth = _clear.m_depth;

					vertex->m_x = -1.0f;
					vertex->m_y = -1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x =  1.0f;
					vertex->m_y = -1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x = -1.0f;
					vertex->m_y =  1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x =  1.0f;
					vertex->m_y =  1.0f;
					vertex->m_z = depth;
				}

				m_vertexBuffers[_clearQuad.m_vb->handle.idx].update(0, 4*_clearQuad.m_decl.m_stride, _clearQuad.m_vb->data);
				deviceCtx->IASetVertexBuffers(0, 1, &vb.m_ptr, &stride, &offset);
				setInputLayout(vertexDecl, program, 0);

				deviceCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				deviceCtx->Draw(4, 0);
			}
		}

		void* m_d3d9dll;
		void* m_d3d11dll;
		void* m_dxgidll;
		void* m_dxgidebugdll;

		void* m_renderdocdll;
		void* m_agsdll;
		AGSContext* m_ags;


		D3D_DRIVER_TYPE   m_driverType;
		D3D_FEATURE_LEVEL m_featureLevel;
		IDXGIAdapter*     m_adapter;
		DXGI_ADAPTER_DESC m_adapterDesc;
#if BX_PLATFORM_WINRT
		IDXGIFactory2*    m_factory;
		IDXGISwapChain1*  m_swapChain;
#else
		IDXGIFactory*     m_factory;
		IDXGISwapChain*   m_swapChain;
#endif // BX_PLATFORM_WINRT

		uint16_t m_lost;
		uint16_t m_numWindows;
		FrameBufferHandle m_windows[BGFX_CONFIG_MAX_FRAME_BUFFERS];

		ID3D11Device*        m_device;
		ID3D11DeviceContext* m_deviceCtx;
		ID3D11InfoQueue*     m_infoQueue;
		TimerQueryD3D11      m_gpuTimer;

		ID3D11RenderTargetView* m_backBufferColor;
		ID3D11DepthStencilView* m_backBufferDepthStencil;
		ID3D11RenderTargetView* m_currentColor;
		ID3D11DepthStencilView* m_currentDepthStencil;

		ID3D11Texture2D* m_captureTexture;
		ID3D11Texture2D* m_captureResolve;

		Resolution m_resolution;
		bool m_wireframe;

#if BX_PLATFORM_WINRT
		typedef DXGI_SWAP_CHAIN_DESC1 SwapChainDesc;
#else
		typedef DXGI_SWAP_CHAIN_DESC  SwapChainDesc;
#endif // BX_PLATFORM_WINRT

		SwapChainDesc m_scd;
		uint32_t m_flags;
		uint32_t m_maxAnisotropy;

		IndexBufferD3D11 m_indexBuffers[BGFX_CONFIG_MAX_INDEX_BUFFERS];
		VertexBufferD3D11 m_vertexBuffers[BGFX_CONFIG_MAX_VERTEX_BUFFERS];
		ShaderD3D11 m_shaders[BGFX_CONFIG_MAX_SHADERS];
		ProgramD3D11 m_program[BGFX_CONFIG_MAX_PROGRAMS];
		TextureD3D11 m_textures[BGFX_CONFIG_MAX_TEXTURES];
		VertexDecl m_vertexDecls[BGFX_CONFIG_MAX_VERTEX_DECLS];
		FrameBufferD3D11 m_frameBuffers[BGFX_CONFIG_MAX_FRAME_BUFFERS];
		void* m_uniforms[BGFX_CONFIG_MAX_UNIFORMS];
		Matrix4 m_predefinedUniforms[PredefinedUniform::Count];
		UniformRegistry m_uniformReg;
		ViewState m_viewState;

		StateCacheT<ID3D11BlendState> m_blendStateCache;
		StateCacheT<ID3D11DepthStencilState> m_depthStencilStateCache;
		StateCacheT<ID3D11InputLayout> m_inputLayoutCache;
		StateCacheT<ID3D11RasterizerState> m_rasterizerStateCache;
		StateCacheT<ID3D11SamplerState> m_samplerStateCache;
		StateCacheLru<IUnknown*, 1024> m_srvUavLru;

		TextVideoMem m_textVideoMem;

		TextureStage m_textureStage;

		ProgramD3D11* m_currentProgram;

		uint8_t m_vsScratch[64<<10];
		uint8_t m_fsScratch[64<<10];
		uint32_t m_vsChanges;
		uint32_t m_fsChanges;

		FrameBufferHandle m_fbh;
		bool m_rtMsaa;

		OVR m_ovr;
		TextureD3D11 m_ovrRT;
		ID3D11RenderTargetView* m_ovrRtv;
		ID3D11DepthStencilView* m_ovrDsv;
	};

	static RendererContextD3D11* s_renderD3D11;

	RendererContextI* rendererCreate()
	{
		s_renderD3D11 = BX_NEW(g_allocator, RendererContextD3D11);
		if (!s_renderD3D11->init() )
		{
			BX_DELETE(g_allocator, s_renderD3D11);
			s_renderD3D11 = NULL;
		}
		return s_renderD3D11;
	}

	void rendererDestroy()
	{
		s_renderD3D11->shutdown();
		BX_DELETE(g_allocator, s_renderD3D11);
		s_renderD3D11 = NULL;
	}

	void stubMultiDrawInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride)
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;
		for (uint32_t ii = 0; ii < _numDrawIndirect; ++ii)
		{
			deviceCtx->DrawInstancedIndirect(_ptr, _offset);
			_offset += _stride;
		}
	}

	void stubMultiDrawIndexedInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride)
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;
		for (uint32_t ii = 0; ii < _numDrawIndirect; ++ii)
		{
			deviceCtx->DrawIndexedInstancedIndirect(_ptr, _offset);
			_offset += _stride;
		}
	}

	void amdAgsMultiDrawInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride)
	{
		agsDriverExtensions_MultiDrawInstancedIndirect(s_renderD3D11->m_ags, _numDrawIndirect, _ptr, _offset, _stride);
	}

	void amdAgsMultiDrawIndexedInstancedIndirect(uint32_t _numDrawIndirect, ID3D11Buffer* _ptr, uint32_t _offset, uint32_t _stride)
	{
		agsDriverExtensions_MultiDrawIndexedInstancedIndirect(s_renderD3D11->m_ags, _numDrawIndirect, _ptr, _offset, _stride);
	}

	struct UavFormat
	{
		DXGI_FORMAT format[3];
		uint32_t    stride;
	};

	static const UavFormat s_uavFormat[] =
	{	//  BGFX_BUFFER_COMPUTE_TYPE_UINT, BGFX_BUFFER_COMPUTE_TYPE_INT,   BGFX_BUFFER_COMPUTE_TYPE_FLOAT
		{ { DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN            },  0 }, // ignored
		{ { DXGI_FORMAT_R8_SINT,           DXGI_FORMAT_R8_UINT,            DXGI_FORMAT_UNKNOWN            },  1 }, // BGFX_BUFFER_COMPUTE_FORMAT_8x1
		{ { DXGI_FORMAT_R8G8_SINT,         DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_UNKNOWN            },  2 }, // BGFX_BUFFER_COMPUTE_FORMAT_8x2
		{ { DXGI_FORMAT_R8G8B8A8_SINT,     DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_UNKNOWN            },  4 }, // BGFX_BUFFER_COMPUTE_FORMAT_8x4
		{ { DXGI_FORMAT_R16_SINT,          DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_R16_FLOAT          },  2 }, // BGFX_BUFFER_COMPUTE_FORMAT_16x1
		{ { DXGI_FORMAT_R16G16_SINT,       DXGI_FORMAT_R16G16_UINT,        DXGI_FORMAT_R16G16_FLOAT       },  4 }, // BGFX_BUFFER_COMPUTE_FORMAT_16x2
		{ { DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_UINT,  DXGI_FORMAT_R16G16B16A16_FLOAT },  8 }, // BGFX_BUFFER_COMPUTE_FORMAT_16x4
		{ { DXGI_FORMAT_R32_SINT,          DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_FLOAT          },  4 }, // BGFX_BUFFER_COMPUTE_FORMAT_32x1
		{ { DXGI_FORMAT_R32G32_SINT,       DXGI_FORMAT_R32G32_UINT,        DXGI_FORMAT_R32G32_FLOAT       },  8 }, // BGFX_BUFFER_COMPUTE_FORMAT_32x2
		{ { DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_UINT,  DXGI_FORMAT_R32G32B32A32_FLOAT }, 16 }, // BGFX_BUFFER_COMPUTE_FORMAT_32x4
	};

	void BufferD3D11::create(uint32_t _size, void* _data, uint16_t _flags, uint16_t _stride, bool _vertex)
	{
		m_uav   = NULL;
		m_size  = _size;
		m_flags = _flags;

		const bool needUav = 0 != (_flags & (BGFX_BUFFER_COMPUTE_WRITE|BGFX_BUFFER_DRAW_INDIRECT) );
		const bool needSrv = 0 != (_flags & BGFX_BUFFER_COMPUTE_READ);
		const bool drawIndirect = 0 != (_flags & BGFX_BUFFER_DRAW_INDIRECT);
		m_dynamic = NULL == _data && !needUav;

		D3D11_BUFFER_DESC desc;
		desc.ByteWidth = _size;
		desc.BindFlags = 0
			| (_vertex ? D3D11_BIND_VERTEX_BUFFER    : D3D11_BIND_INDEX_BUFFER)
			| (needUav ? D3D11_BIND_UNORDERED_ACCESS : 0)
			| (needSrv ? D3D11_BIND_SHADER_RESOURCE  : 0)
			;
		desc.MiscFlags = 0
			| (drawIndirect ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0)
			;
		desc.StructureByteStride = 0;

		DXGI_FORMAT format;
		uint32_t    stride;

		if (drawIndirect)
		{
			format = DXGI_FORMAT_R32G32B32A32_UINT;
			stride = 16;
		}
		else
		{
			uint32_t uavFormat = (_flags & BGFX_BUFFER_COMPUTE_FORMAT_MASK) >> BGFX_BUFFER_COMPUTE_FORMAT_SHIFT;
			if (0 == uavFormat)
			{
				if (_vertex)
				{
					format = DXGI_FORMAT_R32G32B32A32_FLOAT;
					stride = 16;
				}
				else
				{
					if (0 == (_flags & BGFX_BUFFER_INDEX32) )
					{
						format = DXGI_FORMAT_R16_UINT;
						stride = 2;
					}
					else
					{
						format = DXGI_FORMAT_R32_UINT;
						stride = 4;
					}
				}
			}
			else
			{
				const uint32_t uavType = bx::uint32_satsub( (_flags & BGFX_BUFFER_COMPUTE_TYPE_MASK  ) >> BGFX_BUFFER_COMPUTE_TYPE_SHIFT, 1);
				format = s_uavFormat[uavFormat].format[uavType];
				stride = s_uavFormat[uavFormat].stride;
			}
		}

		ID3D11Device* device = s_renderD3D11->m_device;

		if (needUav)
		{
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.CPUAccessFlags = 0;
			desc.StructureByteStride = _stride;

			DX_CHECK(device->CreateBuffer(&desc
				, NULL
				, &m_ptr
				) );

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavd;
			uavd.Format = format;
			uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavd.Buffer.FirstElement = 0;
			uavd.Buffer.NumElements = m_size / stride;
			uavd.Buffer.Flags = 0;
			DX_CHECK(device->CreateUnorderedAccessView(m_ptr
				, &uavd
				, &m_uav
				) );
		}
		else if (m_dynamic)
		{
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			DX_CHECK(device->CreateBuffer(&desc
				, NULL
				, &m_ptr
				) );
		}
		else
		{
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.CPUAccessFlags = 0;

			D3D11_SUBRESOURCE_DATA srd;
			srd.pSysMem = _data;
			srd.SysMemPitch = 0;
			srd.SysMemSlicePitch = 0;

			DX_CHECK(device->CreateBuffer(&desc
				, &srd
				, &m_ptr
				) );
		}

		if (needSrv)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
			srvd.Format = format;
			srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvd.Buffer.FirstElement = 0;
			srvd.Buffer.NumElements = m_size / stride;
			DX_CHECK(device->CreateShaderResourceView(m_ptr
				, &srvd
				, &m_srv
				) );
		}
	}

	void BufferD3D11::update(uint32_t _offset, uint32_t _size, void* _data, bool _discard)
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;
		BX_CHECK(m_dynamic, "Must be dynamic!");

#if 0
		BX_UNUSED(_discard);
		ID3D11Device* device = s_renderD3D11->m_device;

		D3D11_BUFFER_DESC desc;
		desc.ByteWidth = _size;
		desc.Usage     = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.StructureByteStride = 0;

		D3D11_SUBRESOURCE_DATA srd;
		srd.pSysMem     = _data;
		srd.SysMemPitch = 0;
		srd.SysMemSlicePitch = 0;

		ID3D11Buffer* ptr;
		DX_CHECK(device->CreateBuffer(&desc, &srd, &ptr) );

		D3D11_BOX box;
		box.left   = 0;
		box.top    = 0;
		box.front  = 0;
		box.right  = _size;
		box.bottom = 1;
		box.back   = 1;

		deviceCtx->CopySubresourceRegion(m_ptr
			, 0
			, _offset
			, 0
			, 0
			, ptr
			, 0
			, &box
			);

		DX_RELEASE(ptr, 0);
#else
		D3D11_MAPPED_SUBRESOURCE mapped;
		BX_UNUSED(_discard);
		D3D11_MAP type = D3D11_MAP_WRITE_DISCARD;
		DX_CHECK(deviceCtx->Map(m_ptr, 0, type, 0, &mapped) );
		memcpy( (uint8_t*)mapped.pData + _offset, _data, _size);
		deviceCtx->Unmap(m_ptr, 0);
#endif // 0
	}

	void VertexBufferD3D11::create(uint32_t _size, void* _data, VertexDeclHandle _declHandle, uint16_t _flags)
	{
		m_decl = _declHandle;
		uint16_t stride = isValid(_declHandle)
			? s_renderD3D11->m_vertexDecls[_declHandle.idx].m_stride
			: 0
			;

		BufferD3D11::create(_size, _data, _flags, stride, true);
	}

	void ShaderD3D11::create(const Memory* _mem)
	{
		bx::MemoryReader reader(_mem->data, _mem->size);

		uint32_t magic;
		bx::read(&reader, magic);

		switch (magic)
		{
		case BGFX_CHUNK_MAGIC_CSH:
		case BGFX_CHUNK_MAGIC_FSH:
		case BGFX_CHUNK_MAGIC_VSH:
			break;

		default:
			BGFX_FATAL(false, Fatal::InvalidShader, "Unknown shader format %x.", magic);
			break;
		}

		bool fragment = BGFX_CHUNK_MAGIC_FSH == magic;

		uint32_t iohash;
		bx::read(&reader, iohash);

		uint16_t count;
		bx::read(&reader, count);

		m_numPredefined = 0;
		m_numUniforms = count;

		BX_TRACE("%s Shader consts %d"
			, BGFX_CHUNK_MAGIC_FSH == magic ? "Fragment" : BGFX_CHUNK_MAGIC_VSH == magic ? "Vertex" : "Compute"
			, count
			);

		uint8_t fragmentBit = fragment ? BGFX_UNIFORM_FRAGMENTBIT : 0;

		if (0 < count)
		{
			for (uint32_t ii = 0; ii < count; ++ii)
			{
				uint8_t nameSize;
				bx::read(&reader, nameSize);

				char name[256];
				bx::read(&reader, &name, nameSize);
				name[nameSize] = '\0';

				uint8_t type;
				bx::read(&reader, type);

				uint8_t num;
				bx::read(&reader, num);

				uint16_t regIndex;
				bx::read(&reader, regIndex);

				uint16_t regCount;
				bx::read(&reader, regCount);

				const char* kind = "invalid";

				PredefinedUniform::Enum predefined = nameToPredefinedUniformEnum(name);
				if (PredefinedUniform::Count != predefined)
				{
					kind = "predefined";
					m_predefined[m_numPredefined].m_loc   = regIndex;
					m_predefined[m_numPredefined].m_count = regCount;
					m_predefined[m_numPredefined].m_type  = uint8_t(predefined|fragmentBit);
					m_numPredefined++;
				}
				else
				{
					const UniformInfo* info = s_renderD3D11->m_uniformReg.find(name);

					if (NULL != info)
					{
						if (NULL == m_constantBuffer)
						{
							m_constantBuffer = ConstantBuffer::create(1024);
						}

						kind = "user";
						m_constantBuffer->writeUniformHandle( (UniformType::Enum)(type|fragmentBit), regIndex, info->m_handle, regCount);
					}
				}

				BX_TRACE("\t%s: %s (%s), num %2d, r.index %3d, r.count %2d"
					, kind
					, name
					, getUniformTypeName(UniformType::Enum(type&~BGFX_UNIFORM_FRAGMENTBIT) )
					, num
					, regIndex
					, regCount
					);
				BX_UNUSED(kind);
			}

			if (NULL != m_constantBuffer)
			{
				m_constantBuffer->finish();
			}
		}

		uint16_t shaderSize;
		bx::read(&reader, shaderSize);

		const DWORD* code = (const DWORD*)reader.getDataPtr();
		bx::skip(&reader, shaderSize+1);

		if (BGFX_CHUNK_MAGIC_FSH == magic)
		{
			DX_CHECK(s_renderD3D11->m_device->CreatePixelShader(code, shaderSize, NULL, &m_pixelShader) );
			BGFX_FATAL(NULL != m_ptr, bgfx::Fatal::InvalidShader, "Failed to create fragment shader.");
		}
		else if (BGFX_CHUNK_MAGIC_VSH == magic)
		{
			m_hash = bx::hashMurmur2A(code, shaderSize);
			m_code = copy(code, shaderSize);

			DX_CHECK(s_renderD3D11->m_device->CreateVertexShader(code, shaderSize, NULL, &m_vertexShader) );
			BGFX_FATAL(NULL != m_ptr, bgfx::Fatal::InvalidShader, "Failed to create vertex shader.");
		}
		else
		{
			DX_CHECK(s_renderD3D11->m_device->CreateComputeShader(code, shaderSize, NULL, &m_computeShader) );
			BGFX_FATAL(NULL != m_ptr, bgfx::Fatal::InvalidShader, "Failed to create compute shader.");
		}

		uint8_t numAttrs;
		bx::read(&reader, numAttrs);

		memset(m_attrMask, 0, sizeof(m_attrMask) );

		for (uint32_t ii = 0; ii < numAttrs; ++ii)
		{
			uint16_t id;
			bx::read(&reader, id);

			Attrib::Enum attr = idToAttrib(id);

			if (Attrib::Count != attr)
			{
				m_attrMask[attr] = UINT16_MAX;
			}
		}

		uint16_t size;
		bx::read(&reader, size);

		if (0 < size)
		{
			D3D11_BUFFER_DESC desc;
			desc.ByteWidth = (size + 0xf) & ~0xf;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.CPUAccessFlags = 0;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			DX_CHECK(s_renderD3D11->m_device->CreateBuffer(&desc, NULL, &m_buffer) );
		}
	}

	void TextureD3D11::create(const Memory* _mem, uint32_t _flags, uint8_t _skip)
	{
		m_sampler = s_renderD3D11->getSamplerState(_flags);

		ImageContainer imageContainer;

		if (imageParse(imageContainer, _mem->data, _mem->size) )
		{
			uint8_t numMips = imageContainer.m_numMips;
			const uint8_t startLod = uint8_t(bx::uint32_min(_skip, numMips-1) );
			numMips -= startLod;
			const ImageBlockInfo& blockInfo = getBlockInfo(TextureFormat::Enum(imageContainer.m_format) );
			const uint32_t textureWidth  = bx::uint32_max(blockInfo.blockWidth,  imageContainer.m_width >>startLod);
			const uint32_t textureHeight = bx::uint32_max(blockInfo.blockHeight, imageContainer.m_height>>startLod);

			m_flags = _flags;
			m_requestedFormat = (uint8_t)imageContainer.m_format;
			m_textureFormat   = (uint8_t)imageContainer.m_format;

			const TextureFormatInfo& tfi = s_textureFormat[m_requestedFormat];
			const bool convert = DXGI_FORMAT_UNKNOWN == tfi.m_fmt;

			uint8_t bpp = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
			if (convert)
			{
				m_textureFormat = (uint8_t)TextureFormat::BGRA8;
				bpp = 32;
			}

			if (imageContainer.m_cubeMap)
			{
				m_type = TextureCube;
			}
			else if (imageContainer.m_depth > 1)
			{
				m_type = Texture3D;
			}
			else
			{
				m_type = Texture2D;
			}

			m_numMips = numMips;

			uint32_t numSrd = numMips*(imageContainer.m_cubeMap ? 6 : 1);
			D3D11_SUBRESOURCE_DATA* srd = (D3D11_SUBRESOURCE_DATA*)alloca(numSrd*sizeof(D3D11_SUBRESOURCE_DATA) );

			uint32_t kk = 0;

			const bool compressed = isCompressed(TextureFormat::Enum(m_textureFormat) );
			const bool swizzle    = TextureFormat::BGRA8 == m_textureFormat && 0 != (m_flags&BGFX_TEXTURE_COMPUTE_WRITE);

			BX_TRACE("Texture %3d: %s (requested: %s), %dx%d%s%s%s."
				, getHandle()
				, getName( (TextureFormat::Enum)m_textureFormat)
				, getName( (TextureFormat::Enum)m_requestedFormat)
				, textureWidth
				, textureHeight
				, imageContainer.m_cubeMap ? "x6" : ""
				, 0 != (m_flags&BGFX_TEXTURE_RT_MASK) ? " (render target)" : ""
				, swizzle ? " (swizzle BGRA8 -> RGBA8)" : ""
				);

			for (uint8_t side = 0, numSides = imageContainer.m_cubeMap ? 6 : 1; side < numSides; ++side)
			{
				uint32_t width  = textureWidth;
				uint32_t height = textureHeight;
				uint32_t depth  = imageContainer.m_depth;

				for (uint8_t lod = 0, num = numMips; lod < num; ++lod)
				{
					width  = bx::uint32_max(1, width);
					height = bx::uint32_max(1, height);
					depth  = bx::uint32_max(1, depth);

					ImageMip mip;
					if (imageGetRawData(imageContainer, side, lod+startLod, _mem->data, _mem->size, mip) )
					{
						srd[kk].pSysMem = mip.m_data;

						if (convert)
						{
							uint32_t srcpitch = mip.m_width*bpp/8;
							uint8_t* temp = (uint8_t*)BX_ALLOC(g_allocator, mip.m_width*mip.m_height*bpp/8);
							imageDecodeToBgra8(temp, mip.m_data, mip.m_width, mip.m_height, srcpitch, mip.m_format);

							srd[kk].pSysMem = temp;
							srd[kk].SysMemPitch = srcpitch;
						}
						else if (compressed)
						{
							srd[kk].SysMemPitch      = (mip.m_width /blockInfo.blockWidth )*mip.m_blockSize;
							srd[kk].SysMemSlicePitch = (mip.m_height/blockInfo.blockHeight)*srd[kk].SysMemPitch;
						}
						else
						{
							srd[kk].SysMemPitch = mip.m_width*mip.m_bpp/8;
						}

 						if (swizzle)
 						{
// 							imageSwizzleBgra8(width, height, mip.m_width*4, data, temp);
 						}

						srd[kk].SysMemSlicePitch = mip.m_height*srd[kk].SysMemPitch;
						++kk;
					}

					width  >>= 1;
					height >>= 1;
					depth  >>= 1;
				}
			}

			const bool bufferOnly   = 0 != (m_flags&BGFX_TEXTURE_RT_BUFFER_ONLY);
			const bool computeWrite = 0 != (m_flags&BGFX_TEXTURE_COMPUTE_WRITE);
			const bool renderTarget = 0 != (m_flags&BGFX_TEXTURE_RT_MASK);
			const bool srgb			= 0 != (m_flags&BGFX_TEXTURE_SRGB) || imageContainer.m_srgb;
			const uint32_t msaaQuality = bx::uint32_satsub( (m_flags&BGFX_TEXTURE_RT_MSAA_MASK)>>BGFX_TEXTURE_RT_MSAA_SHIFT, 1);
			const DXGI_SAMPLE_DESC& msaa = s_msaa[msaaQuality];

			D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
			memset(&srvd, 0, sizeof(srvd) );

			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			if (swizzle)
			{
				format      = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
				srvd.Format = format;
			}
			else if (srgb)
			{
				format      = s_textureFormat[m_textureFormat].m_fmtSrgb;
				srvd.Format = format;
				BX_WARN(format != DXGI_FORMAT_UNKNOWN, "sRGB not supported for texture format %d", m_textureFormat);
			}

			if (format == DXGI_FORMAT_UNKNOWN)
			{
				// not swizzled and not sRGB, or sRGB unsupported
				format		= s_textureFormat[m_textureFormat].m_fmt;
				srvd.Format = s_textureFormat[m_textureFormat].m_fmtSrv;
			}

			switch (m_type)
			{
			case Texture2D:
			case TextureCube:
				{
					D3D11_TEXTURE2D_DESC desc;
					desc.Width  = textureWidth;
					desc.Height = textureHeight;
					desc.MipLevels  = numMips;
					desc.Format     = format;
					desc.SampleDesc = msaa;
					desc.Usage      = kk == 0 ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
					desc.BindFlags  = bufferOnly ? 0 : D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = 0;

					if (isDepth( (TextureFormat::Enum)m_textureFormat) )
					{
						desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
						desc.Usage = D3D11_USAGE_DEFAULT;
					}
					else if (renderTarget)
					{
						desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
						desc.Usage = D3D11_USAGE_DEFAULT;
					}

					if (computeWrite)
					{
						desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
						desc.Usage = D3D11_USAGE_DEFAULT;
					}

					if (imageContainer.m_cubeMap)
					{
						desc.ArraySize = 6;
						desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
						srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
						srvd.TextureCube.MipLevels = numMips;
					}
					else
					{
						desc.ArraySize = 1;
						desc.MiscFlags = 0;
						srvd.ViewDimension = 1 < msaa.Count ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
						srvd.Texture2D.MipLevels = numMips;
					}

					DX_CHECK(s_renderD3D11->m_device->CreateTexture2D(&desc, kk == 0 ? NULL : srd, &m_texture2d) );
				}
				break;

			case Texture3D:
				{
					D3D11_TEXTURE3D_DESC desc;
					desc.Width  = textureWidth;
					desc.Height = textureHeight;
					desc.Depth  = imageContainer.m_depth;
					desc.MipLevels = imageContainer.m_numMips;
					desc.Format    = format;
					desc.Usage     = kk == 0 ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = 0;
					desc.MiscFlags      = 0;

					if (computeWrite)
					{
						desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
						desc.Usage = D3D11_USAGE_DEFAULT;
					}

					srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
					srvd.Texture3D.MipLevels = numMips;

					DX_CHECK(s_renderD3D11->m_device->CreateTexture3D(&desc, kk == 0 ? NULL : srd, &m_texture3d) );
				}
				break;
			}

			if (!bufferOnly)
			{
				DX_CHECK(s_renderD3D11->m_device->CreateShaderResourceView(m_ptr, &srvd, &m_srv) );
			}

			if (computeWrite)
			{
				DX_CHECK(s_renderD3D11->m_device->CreateUnorderedAccessView(m_ptr, NULL, &m_uav) );
			}

			if (convert
			&&  0 != kk)
			{
				kk = 0;
				for (uint8_t side = 0, numSides = imageContainer.m_cubeMap ? 6 : 1; side < numSides; ++side)
				{
					for (uint32_t lod = 0, num = numMips; lod < num; ++lod)
					{
						BX_FREE(g_allocator, const_cast<void*>(srd[kk].pSysMem) );
						++kk;
					}
				}
			}
		}
	}

	void TextureD3D11::destroy()
	{
		s_renderD3D11->m_srvUavLru.invalidateWithParent(getHandle().idx);
		DX_RELEASE(m_srv, 0);
		DX_RELEASE(m_uav, 0);
		DX_RELEASE(m_ptr, 0);
	}

	void TextureD3D11::update(uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem)
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;

		D3D11_BOX box;
		box.left   = _rect.m_x;
		box.top    = _rect.m_y;
		box.right  = box.left + _rect.m_width;
		box.bottom = box.top + _rect.m_height;
		box.front  = _z;
		box.back   = box.front + _depth;

		const uint32_t subres = _mip + (_side * m_numMips);
		const uint32_t bpp    = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
		const uint32_t rectpitch = _rect.m_width*bpp/8;
		const uint32_t srcpitch  = UINT16_MAX == _pitch ? rectpitch : _pitch;

		const bool convert = m_textureFormat != m_requestedFormat;

		uint8_t* data = _mem->data;
		uint8_t* temp = NULL;

		if (convert)
		{
			temp = (uint8_t*)BX_ALLOC(g_allocator, rectpitch*_rect.m_height);
			imageDecodeToBgra8(temp, data, _rect.m_width, _rect.m_height, srcpitch, m_requestedFormat);
			data = temp;
		}

		deviceCtx->UpdateSubresource(m_ptr, subres, &box, data, srcpitch, 0);

		if (NULL != temp)
		{
			BX_FREE(g_allocator, temp);
		}
	}

	void TextureD3D11::commit(uint8_t _stage, uint32_t _flags)
	{
		TextureStage& ts = s_renderD3D11->m_textureStage;
		ts.m_srv[_stage] = m_srv;
		ts.m_sampler[_stage] = 0 == (BGFX_SAMPLER_DEFAULT_FLAGS & _flags)
			? s_renderD3D11->getSamplerState(_flags)
			: m_sampler
			;
	}

	void TextureD3D11::resolve()
	{
	}

	TextureHandle TextureD3D11::getHandle() const
	{
		TextureHandle handle = { (uint16_t)(this - s_renderD3D11->m_textures) };
		return handle;
	}

	void FrameBufferD3D11::create(uint8_t _num, const TextureHandle* _handles)
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_rtv); ++ii)
		{
			m_rtv[ii] = NULL;
		}
		m_dsv       = NULL;
		m_swapChain = NULL;

		m_numTh = _num;
		memcpy(m_th, _handles, _num*sizeof(TextureHandle) );

		postReset();
	}

	void FrameBufferD3D11::create(uint16_t _denseIdx, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat)
	{
		DXGI_SWAP_CHAIN_DESC scd;
		memcpy(&scd, &s_renderD3D11->m_scd, sizeof(DXGI_SWAP_CHAIN_DESC) );
		scd.BufferDesc.Width  = _width;
		scd.BufferDesc.Height = _height;
		scd.OutputWindow = (HWND)_nwh;

		ID3D11Device* device = s_renderD3D11->m_device;

		HRESULT hr;
		hr = s_renderD3D11->m_factory->CreateSwapChain(device
			, &scd
			, &m_swapChain
			);
		BGFX_FATAL(SUCCEEDED(hr), Fatal::UnableToInitialize, "Failed to create swap chain.");

		ID3D11Resource* ptr;
		DX_CHECK(m_swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&ptr) );
		DX_CHECK(device->CreateRenderTargetView(ptr, NULL, &m_rtv[0]) );
		DX_RELEASE(ptr, 0);

		DXGI_FORMAT fmtDsv = isDepth(_depthFormat)
			? s_textureFormat[_depthFormat].m_fmtDsv
			: DXGI_FORMAT_D24_UNORM_S8_UINT
			;
		D3D11_TEXTURE2D_DESC dsd;
		dsd.Width  = scd.BufferDesc.Width;
		dsd.Height = scd.BufferDesc.Height;
		dsd.MipLevels  = 1;
		dsd.ArraySize  = 1;
		dsd.Format     = fmtDsv;
		dsd.SampleDesc = scd.SampleDesc;
		dsd.Usage      = D3D11_USAGE_DEFAULT;
		dsd.BindFlags  = D3D11_BIND_DEPTH_STENCIL;
		dsd.CPUAccessFlags = 0;
		dsd.MiscFlags      = 0;

		ID3D11Texture2D* depthStencil;
		DX_CHECK(device->CreateTexture2D(&dsd, NULL, &depthStencil) );
		DX_CHECK(device->CreateDepthStencilView(depthStencil, NULL, &m_dsv) );
		DX_RELEASE(depthStencil, 0);

		m_srv[0]   = NULL;
		m_denseIdx = _denseIdx;
		m_num      = 1;
	}

	uint16_t FrameBufferD3D11::destroy()
	{
		preReset(true);

		DX_RELEASE(m_swapChain, 0);

		m_num   = 0;
		m_numTh = 0;

		uint16_t denseIdx = m_denseIdx;
		m_denseIdx = UINT16_MAX;

		return denseIdx;
	}

	void FrameBufferD3D11::preReset(bool _force)
	{
		if (0 < m_numTh
		||  _force)
		{
			for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
			{
				DX_RELEASE(m_srv[ii], 0);
				DX_RELEASE(m_rtv[ii], 0);
			}

			DX_RELEASE(m_dsv, 0);
		}
	}

	void FrameBufferD3D11::postReset()
	{
		if (0 < m_numTh)
		{
			m_num = 0;
			for (uint32_t ii = 0; ii < m_numTh; ++ii)
			{
				TextureHandle handle = m_th[ii];
				if (isValid(handle) )
				{
					const TextureD3D11& texture = s_renderD3D11->m_textures[handle.idx];
					if (isDepth( (TextureFormat::Enum)texture.m_textureFormat) )
					{
						BX_CHECK(NULL == m_dsv, "Frame buffer already has depth-stencil attached.");

						const uint32_t msaaQuality = bx::uint32_satsub( (texture.m_flags&BGFX_TEXTURE_RT_MSAA_MASK)>>BGFX_TEXTURE_RT_MSAA_SHIFT, 1);
						const DXGI_SAMPLE_DESC& msaa = s_msaa[msaaQuality];

						D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
						dsvDesc.Format = s_textureFormat[texture.m_textureFormat].m_fmtDsv;
						dsvDesc.ViewDimension = 1 < msaa.Count ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
						dsvDesc.Flags = 0;
						dsvDesc.Texture2D.MipSlice = 0;
						DX_CHECK(s_renderD3D11->m_device->CreateDepthStencilView(texture.m_ptr, &dsvDesc, &m_dsv) );
					}
					else
					{
						DX_CHECK(s_renderD3D11->m_device->CreateRenderTargetView(texture.m_ptr, NULL, &m_rtv[m_num]) );
						DX_CHECK(s_renderD3D11->m_device->CreateShaderResourceView(texture.m_ptr, NULL, &m_srv[m_num]) );
						m_num++;
					}
				}
			}
		}
	}

	void FrameBufferD3D11::resolve()
	{
	}

	void FrameBufferD3D11::clear(const Clear& _clear, const float _palette[][4])
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;

		if (BGFX_CLEAR_COLOR & _clear.m_flags)
		{
			if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
			{
				for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
				{
					uint8_t index = _clear.m_index[ii];
					if (NULL != m_rtv[ii]
					&&  UINT8_MAX != index)
					{
						deviceCtx->ClearRenderTargetView(m_rtv[ii], _palette[index]);
					}
				}
			}
			else
			{
				float frgba[4] =
				{
					_clear.m_index[0]*1.0f/255.0f,
					_clear.m_index[1]*1.0f/255.0f,
					_clear.m_index[2]*1.0f/255.0f,
					_clear.m_index[3]*1.0f/255.0f,
				};
				for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
				{
					if (NULL != m_rtv[ii])
					{
						deviceCtx->ClearRenderTargetView(m_rtv[ii], frgba);
					}
				}
			}
		}

		if (NULL != m_dsv
		&& (BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL) & _clear.m_flags)
		{
			DWORD flags = 0;
			flags |= (_clear.m_flags & BGFX_CLEAR_DEPTH) ? D3D11_CLEAR_DEPTH : 0;
			flags |= (_clear.m_flags & BGFX_CLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0;
			deviceCtx->ClearDepthStencilView(m_dsv, flags, _clear.m_depth, _clear.m_stencil);
		}
	}

	void TimerQueryD3D11::postReset()
	{
		ID3D11Device* device = s_renderD3D11->m_device;

		D3D11_QUERY_DESC query;
		query.MiscFlags = 0;
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_frame); ++ii)
		{
			Frame& frame = m_frame[ii];

			query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
			DX_CHECK(device->CreateQuery(&query, &frame.m_disjoint) );

			query.Query = D3D11_QUERY_TIMESTAMP;
			DX_CHECK(device->CreateQuery(&query, &frame.m_start) );
			DX_CHECK(device->CreateQuery(&query, &frame.m_end) );
		}

		m_elapsed   = 0;
		m_frequency = 1;
		m_control.reset();
	}

	void TimerQueryD3D11::preReset()
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_frame); ++ii)
		{
			Frame& frame = m_frame[ii];
			DX_RELEASE(frame.m_disjoint, 0);
			DX_RELEASE(frame.m_start, 0);
			DX_RELEASE(frame.m_end, 0);
		}
	}

	void TimerQueryD3D11::begin()
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;

		while (0 == m_control.reserve(1) )
		{
			get();
		}

		Frame& frame = m_frame[m_control.m_current];
		deviceCtx->Begin(frame.m_disjoint);
		deviceCtx->End(frame.m_start);
	}

	void TimerQueryD3D11::end()
	{
		ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;
		Frame& frame = m_frame[m_control.m_current];
		deviceCtx->End(frame.m_end);
		deviceCtx->End(frame.m_disjoint);
		m_control.commit(1);
	}

	bool TimerQueryD3D11::get()
	{
		if (0 != m_control.available() )
		{
			ID3D11DeviceContext* deviceCtx = s_renderD3D11->m_deviceCtx;
			Frame& frame = m_frame[m_control.m_read];

			uint64_t finish;
			HRESULT hr = deviceCtx->GetData(frame.m_end, &finish, sizeof(finish), 0);
			if (S_OK == hr)
			{
				m_control.consume(1);

				struct D3D11_QUERY_DATA_TIMESTAMP_DISJOINT
				{
					UINT64 Frequency;
					BOOL Disjoint;
				};

				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
				deviceCtx->GetData(frame.m_disjoint, &disjoint, sizeof(disjoint), 0);

				uint64_t start;
				deviceCtx->GetData(frame.m_start, &start, sizeof(start), 0);

				m_frequency = disjoint.Frequency;
				m_elapsed   = finish - start;

				return true;
			}
		}

		return false;
	}

	void RendererContextD3D11::submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter)
	{
		PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), L"rendererSubmit");

		ID3D11DeviceContext* deviceCtx = m_deviceCtx;

		updateResolution(_render->m_resolution);

		int64_t elapsed = -bx::getHPCounter();
		int64_t captureElapsed = 0;

		m_gpuTimer.begin();

		if (0 < _render->m_iboffset)
		{
			TransientIndexBuffer* ib = _render->m_transientIb;
			m_indexBuffers[ib->handle.idx].update(0, _render->m_iboffset, ib->data);
		}

		if (0 < _render->m_vboffset)
		{
			TransientVertexBuffer* vb = _render->m_transientVb;
			m_vertexBuffers[vb->handle.idx].update(0, _render->m_vboffset, vb->data);
		}

		_render->sort();

		RenderDraw currentState;
		currentState.clear();
		currentState.m_flags = BGFX_STATE_NONE;
		currentState.m_stencil = packStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);

		_render->m_hmdInitialized = m_ovr.isInitialized();

		const bool hmdEnabled = m_ovr.isEnabled() || m_ovr.isDebug();
		ViewState& viewState = m_viewState;
		viewState.reset(_render, hmdEnabled);

		bool wireframe = !!(_render->m_debug&BGFX_DEBUG_WIREFRAME);
		bool scissorEnabled = false;
		setDebugWireframe(wireframe);

		uint16_t programIdx = invalidHandle;
		SortKey key;
		uint16_t view = UINT16_MAX;
		FrameBufferHandle fbh = BGFX_INVALID_HANDLE;

		const uint64_t primType = _render->m_debug&BGFX_DEBUG_WIREFRAME ? BGFX_STATE_PT_LINES : 0;
		uint8_t primIndex = uint8_t(primType >> BGFX_STATE_PT_SHIFT);
		PrimInfo prim = s_primInfo[primIndex];
		deviceCtx->IASetPrimitiveTopology(prim.m_type);

		bool wasCompute = false;
		bool viewHasScissor = false;
		Rect viewScissorRect;
		viewScissorRect.clear();

		uint32_t statsNumPrimsSubmitted[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumPrimsRendered[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumInstances[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumDrawIndirect[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumIndices = 0;
		uint32_t statsKeyType[2] = {};

		if (0 == (_render->m_debug&BGFX_DEBUG_IFH) )
		{
			// reset the framebuffer to be the backbuffer; depending on the swap effect,
			// if we don't do this we'll only see one frame of output and then nothing
			setFrameBuffer(fbh);

			bool viewRestart = false;
			uint8_t eye = 0;
			uint8_t restartState = 0;
			viewState.m_rect = _render->m_rect[0];

			int32_t numItems = _render->m_num;
			for (int32_t item = 0, restartItem = numItems; item < numItems || restartItem < numItems;)
			{
				const bool isCompute = key.decode(_render->m_sortKeys[item], _render->m_viewRemap);
				statsKeyType[isCompute]++;

				const bool viewChanged = 0
					|| key.m_view != view
					|| item == numItems
					;

				const RenderItem& renderItem = _render->m_renderItem[_render->m_sortValues[item] ];
				++item;

				if (viewChanged)
				{
					if (1 == restartState)
					{
						restartState = 2;
						item = restartItem;
						restartItem = numItems;
						view = UINT16_MAX;
						continue;
					}

					view = key.m_view;
					programIdx = invalidHandle;

					if (_render->m_fb[view].idx != fbh.idx)
					{
						fbh = _render->m_fb[view];
						setFrameBuffer(fbh);
					}

					viewRestart = ( (BGFX_VIEW_STEREO == (_render->m_viewFlags[view] & BGFX_VIEW_STEREO) ) );
					viewRestart &= hmdEnabled;
					if (viewRestart)
					{
						if (0 == restartState)
						{
							restartState = 1;
							restartItem  = item - 1;
						}

						eye = (restartState - 1) & 1;
						restartState &= 1;
					}
					else
					{
						eye = 0;
					}

					PIX_ENDEVENT();

					viewState.m_rect = _render->m_rect[view];
					if (viewRestart)
					{
						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
						{
							wchar_t* viewNameW = s_viewNameW[view];
							viewNameW[3] = L' ';
							viewNameW[4] = eye ? L'R' : L'L';
							PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), viewNameW);
						}

						if (m_ovr.isEnabled() )
						{
							m_ovr.getViewport(eye, &viewState.m_rect);
						}
						else
						{
							viewState.m_rect.m_x = eye * (viewState.m_rect.m_width+1)/2;
							viewState.m_rect.m_width /= 2;
						}
					}
					else
					{
						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
						{
							wchar_t* viewNameW = s_viewNameW[view];
							viewNameW[3] = L' ';
							viewNameW[4] = L' ';
							PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), viewNameW);
						}
					}

					const Rect& scissorRect = _render->m_scissor[view];
					viewHasScissor = !scissorRect.isZero();
					viewScissorRect = viewHasScissor ? scissorRect : viewState.m_rect;

					D3D11_VIEWPORT vp;
					vp.TopLeftX = viewState.m_rect.m_x;
					vp.TopLeftY = viewState.m_rect.m_y;
					vp.Width    = viewState.m_rect.m_width;
					vp.Height   = viewState.m_rect.m_height;
					vp.MinDepth = 0.0f;
					vp.MaxDepth = 1.0f;
					deviceCtx->RSSetViewports(1, &vp);
					Clear& clr = _render->m_clear[view];

					if (BGFX_CLEAR_NONE != (clr.m_flags & BGFX_CLEAR_MASK) )
					{
						clearQuad(_clearQuad, viewState.m_rect, clr, _render->m_clearColor);
						prim = s_primInfo[BX_COUNTOF(s_primName)]; // Force primitive type update after clear quad.
					}
				}

				if (isCompute)
				{
					if (!wasCompute)
					{
						wasCompute = true;

						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
						{
							wchar_t* viewNameW = s_viewNameW[view];
							viewNameW[3] = L'C';
							PIX_ENDEVENT();
							PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), viewNameW);
						}

						deviceCtx->IASetVertexBuffers(0, 2, s_zero.m_buffer, s_zero.m_zero, s_zero.m_zero);
						deviceCtx->IASetIndexBuffer(NULL, DXGI_FORMAT_R16_UINT, 0);

						deviceCtx->VSSetShaderResources(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, s_zero.m_srv);
						deviceCtx->PSSetShaderResources(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, s_zero.m_srv);

						deviceCtx->VSSetSamplers(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, s_zero.m_sampler);
						deviceCtx->PSSetSamplers(0, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, s_zero.m_sampler);
					}

					const RenderCompute& compute = renderItem.compute;

					if (0 != eye
					&&  BGFX_SUBMIT_EYE_LEFT == (compute.m_submitFlags&BGFX_SUBMIT_EYE_MASK) )
					{
						continue;
					}

					bool programChanged = false;
					bool constantsChanged = compute.m_constBegin < compute.m_constEnd;
					rendererUpdateUniforms(this, _render->m_constantBuffer, compute.m_constBegin, compute.m_constEnd);

					if (key.m_program != programIdx)
					{
						programIdx = key.m_program;

						ProgramD3D11& program = m_program[key.m_program];
						m_currentProgram = &program;

						deviceCtx->CSSetShader(program.m_vsh->m_computeShader, NULL, 0);
						deviceCtx->CSSetConstantBuffers(0, 1, &program.m_vsh->m_buffer);

						programChanged =
							constantsChanged = true;
					}

					if (invalidHandle != programIdx)
					{
						ProgramD3D11& program = m_program[programIdx];

						if (constantsChanged)
						{
							ConstantBuffer* vcb = program.m_vsh->m_constantBuffer;
							if (NULL != vcb)
							{
								commit(*vcb);
							}
						}

						viewState.setPredefined<4>(this, view, eye, program, _render, compute);

						if (constantsChanged
						||  program.m_numPredefined > 0)
						{
							commitShaderConstants();
						}
					}
					BX_UNUSED(programChanged);
					ID3D11UnorderedAccessView* uav[BGFX_MAX_COMPUTE_BINDINGS] = {};
					ID3D11ShaderResourceView*  srv[BGFX_MAX_COMPUTE_BINDINGS] = {};
					ID3D11SamplerState*    sampler[BGFX_MAX_COMPUTE_BINDINGS] = {};

					for (uint32_t ii = 0; ii < BGFX_MAX_COMPUTE_BINDINGS; ++ii)
					{
						const Binding& bind = compute.m_bind[ii];
						if (invalidHandle != bind.m_idx)
						{
							switch (bind.m_type)
							{
							case Binding::Image:
								{
									TextureD3D11& texture = m_textures[bind.m_idx];
									if (Access::Read != bind.m_un.m_compute.m_access)
									{
										uav[ii] = 0 == bind.m_un.m_compute.m_mip
											? texture.m_uav
											: s_renderD3D11->getCachedUav(texture.getHandle(), bind.m_un.m_compute.m_mip)
											;
									}
									else
									{
										srv[ii] = 0 == bind.m_un.m_compute.m_mip
											? texture.m_srv
											: s_renderD3D11->getCachedSrv(texture.getHandle(), bind.m_un.m_compute.m_mip)
											;
										sampler[ii] = texture.m_sampler;
									}
								}
								break;

							case Binding::IndexBuffer:
							case Binding::VertexBuffer:
								{
									const BufferD3D11& buffer = Binding::IndexBuffer == bind.m_type
										? m_indexBuffers[bind.m_idx]
										: m_vertexBuffers[bind.m_idx]
										;
									if (Access::Read != bind.m_un.m_compute.m_access)
									{
										uav[ii] = buffer.m_uav;
									}
									else
									{
										srv[ii] = buffer.m_srv;
									}
								}
								break;
							}
						}
					}

					deviceCtx->CSSetUnorderedAccessViews(0, BX_COUNTOF(uav), uav, NULL);
					deviceCtx->CSSetShaderResources(0, BX_COUNTOF(srv), srv);
					deviceCtx->CSSetSamplers(0, BX_COUNTOF(sampler), sampler);

					if (isValid(compute.m_indirectBuffer) )
					{
						const VertexBufferD3D11& vb = m_vertexBuffers[compute.m_indirectBuffer.idx];
						ID3D11Buffer* ptr = vb.m_ptr;

						uint32_t numDrawIndirect = UINT16_MAX == compute.m_numIndirect
							? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
							: compute.m_numIndirect
							;

						uint32_t args = compute.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
						for (uint32_t ii = 0; ii < numDrawIndirect; ++ii)
						{
							deviceCtx->DispatchIndirect(ptr, args);
							args += BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
						}
					}
					else
					{
						deviceCtx->Dispatch(compute.m_numX, compute.m_numY, compute.m_numZ);
					}

					continue;
				}

				bool resetState = viewChanged || wasCompute;

				if (wasCompute)
				{
					if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
					{
						wchar_t* viewNameW = s_viewNameW[view];
						viewNameW[3] = L' ';
						PIX_ENDEVENT();
						PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), viewNameW);
					}

					wasCompute = false;

					programIdx = invalidHandle;
					m_currentProgram = NULL;

					invalidateCompute();
				}

				const RenderDraw& draw = renderItem.draw;

				const uint64_t newFlags = draw.m_flags;
				uint64_t changedFlags = currentState.m_flags ^ draw.m_flags;
				changedFlags |= currentState.m_rgba != draw.m_rgba ? BGFX_D3D11_BLEND_STATE_MASK : 0;
				currentState.m_flags = newFlags;

				const uint64_t newStencil = draw.m_stencil;
				uint64_t changedStencil = currentState.m_stencil ^ draw.m_stencil;
				changedFlags |= 0 != changedStencil ? BGFX_D3D11_DEPTH_STENCIL_MASK : 0;
				currentState.m_stencil = newStencil;

				if (resetState)
				{
					currentState.clear();
					currentState.m_scissor = !draw.m_scissor;
					changedFlags = BGFX_STATE_MASK;
					changedStencil = packStencil(BGFX_STENCIL_MASK, BGFX_STENCIL_MASK);
					currentState.m_flags = newFlags;
					currentState.m_stencil = newStencil;

					setBlendState(newFlags);
					setDepthStencilState(newFlags, packStencil(BGFX_STENCIL_DEFAULT, BGFX_STENCIL_DEFAULT) );

					const uint64_t pt = newFlags&BGFX_STATE_PT_MASK;
					primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
				}

				if (prim.m_type != s_primInfo[primIndex].m_type)
				{
					prim = s_primInfo[primIndex];
					deviceCtx->IASetPrimitiveTopology(prim.m_type);
				}

				uint16_t scissor = draw.m_scissor;
				if (currentState.m_scissor != scissor)
				{
					currentState.m_scissor = scissor;

					if (UINT16_MAX == scissor)
					{
						scissorEnabled = viewHasScissor;
						if (viewHasScissor)
						{
							D3D11_RECT rc;
							rc.left   = viewScissorRect.m_x;
							rc.top    = viewScissorRect.m_y;
							rc.right  = viewScissorRect.m_x + viewScissorRect.m_width;
							rc.bottom = viewScissorRect.m_y + viewScissorRect.m_height;
							deviceCtx->RSSetScissorRects(1, &rc);
						}
					}
					else
					{
						Rect scissorRect;
						scissorRect.intersect(viewScissorRect, _render->m_rectCache.m_cache[scissor]);
						scissorEnabled = true;
						D3D11_RECT rc;
						rc.left   = scissorRect.m_x;
						rc.top    = scissorRect.m_y;
						rc.right  = scissorRect.m_x + scissorRect.m_width;
						rc.bottom = scissorRect.m_y + scissorRect.m_height;
						deviceCtx->RSSetScissorRects(1, &rc);
					}

					setRasterizerState(newFlags, wireframe, scissorEnabled);
				}

				if (BGFX_D3D11_DEPTH_STENCIL_MASK & changedFlags)
				{
					setDepthStencilState(newFlags, newStencil);
				}

				if (BGFX_D3D11_BLEND_STATE_MASK & changedFlags)
				{
					setBlendState(newFlags, draw.m_rgba);
					currentState.m_rgba = draw.m_rgba;
				}

				if ( (0
					 | BGFX_STATE_CULL_MASK
					 | BGFX_STATE_ALPHA_REF_MASK
					 | BGFX_STATE_PT_MASK
					 | BGFX_STATE_POINT_SIZE_MASK
					 | BGFX_STATE_MSAA
					 ) & changedFlags)
				{
					if ( (BGFX_STATE_CULL_MASK|BGFX_STATE_MSAA) & changedFlags)
					{
						setRasterizerState(newFlags, wireframe, scissorEnabled);
					}

					if (BGFX_STATE_ALPHA_REF_MASK & changedFlags)
					{
						uint32_t ref = (newFlags&BGFX_STATE_ALPHA_REF_MASK)>>BGFX_STATE_ALPHA_REF_SHIFT;
						viewState.m_alphaRef = ref/255.0f;
					}

					const uint64_t pt = newFlags&BGFX_STATE_PT_MASK;
					primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
					if (prim.m_type != s_primInfo[primIndex].m_type)
					{
						prim = s_primInfo[primIndex];
						deviceCtx->IASetPrimitiveTopology(prim.m_type);
					}
				}

				bool programChanged = false;
				bool constantsChanged = draw.m_constBegin < draw.m_constEnd;
				rendererUpdateUniforms(this, _render->m_constantBuffer, draw.m_constBegin, draw.m_constEnd);

				if (key.m_program != programIdx)
				{
					programIdx = key.m_program;

					if (invalidHandle == programIdx)
					{
						m_currentProgram = NULL;

						deviceCtx->VSSetShader(NULL, NULL, 0);
						deviceCtx->PSSetShader(NULL, NULL, 0);
					}
					else
					{
						ProgramD3D11& program = m_program[programIdx];
						m_currentProgram = &program;

						const ShaderD3D11* vsh = program.m_vsh;
						deviceCtx->VSSetShader(vsh->m_vertexShader, NULL, 0);
						deviceCtx->VSSetConstantBuffers(0, 1, &vsh->m_buffer);

						if (NULL != m_currentColor)
						{
							const ShaderD3D11* fsh = program.m_fsh;
							deviceCtx->PSSetShader(fsh->m_pixelShader, NULL, 0);
							deviceCtx->PSSetConstantBuffers(0, 1, &fsh->m_buffer);
						}
						else
						{
							deviceCtx->PSSetShader(NULL, NULL, 0);
						}
					}

					programChanged =
						constantsChanged = true;
				}

				if (invalidHandle != programIdx)
				{
					ProgramD3D11& program = m_program[programIdx];

					if (constantsChanged)
					{
						ConstantBuffer* vcb = program.m_vsh->m_constantBuffer;
						if (NULL != vcb)
						{
							commit(*vcb);
						}

						ConstantBuffer* fcb = program.m_fsh->m_constantBuffer;
						if (NULL != fcb)
						{
							commit(*fcb);
						}
					}

					viewState.setPredefined<4>(this, view, eye, program, _render, draw);

					if (constantsChanged
					||  program.m_numPredefined > 0)
					{
						commitShaderConstants();
					}
				}

				{
					uint32_t changes = 0;
					for (uint8_t stage = 0; stage < BGFX_CONFIG_MAX_TEXTURE_SAMPLERS; ++stage)
					{
						const Binding& bind = draw.m_bind[stage];
						Binding& current = currentState.m_bind[stage];
						if (current.m_idx != bind.m_idx
						||  current.m_un.m_draw.m_flags != bind.m_un.m_draw.m_flags
						||  programChanged)
						{
							if (invalidHandle != bind.m_idx)
							{
								TextureD3D11& texture = m_textures[bind.m_idx];
								texture.commit(stage, bind.m_un.m_draw.m_flags);
							}
							else
							{
								m_textureStage.m_srv[stage]     = NULL;
								m_textureStage.m_sampler[stage] = NULL;
							}

							++changes;
						}

						current = bind;
					}

					if (0 < changes)
					{
						commitTextureStage();
					}
				}

				if (programChanged
				||  currentState.m_vertexDecl.idx         != draw.m_vertexDecl.idx
				||  currentState.m_vertexBuffer.idx       != draw.m_vertexBuffer.idx
				||  currentState.m_instanceDataBuffer.idx != draw.m_instanceDataBuffer.idx
				||  currentState.m_instanceDataOffset     != draw.m_instanceDataOffset
				||  currentState.m_instanceDataStride     != draw.m_instanceDataStride)
				{
					currentState.m_vertexDecl             = draw.m_vertexDecl;
					currentState.m_vertexBuffer           = draw.m_vertexBuffer;
					currentState.m_instanceDataBuffer.idx = draw.m_instanceDataBuffer.idx;
					currentState.m_instanceDataOffset     = draw.m_instanceDataOffset;
					currentState.m_instanceDataStride     = draw.m_instanceDataStride;

					uint16_t handle = draw.m_vertexBuffer.idx;
					if (invalidHandle != handle)
					{
						const VertexBufferD3D11& vb = m_vertexBuffers[handle];

						uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
						const VertexDecl& vertexDecl = m_vertexDecls[decl];
						uint32_t stride = vertexDecl.m_stride;
						uint32_t offset = 0;
						deviceCtx->IASetVertexBuffers(0, 1, &vb.m_ptr, &stride, &offset);

						if (isValid(draw.m_instanceDataBuffer) )
						{
							const VertexBufferD3D11& inst = m_vertexBuffers[draw.m_instanceDataBuffer.idx];
							uint32_t instStride = draw.m_instanceDataStride;
							deviceCtx->IASetVertexBuffers(1, 1, &inst.m_ptr, &instStride, &draw.m_instanceDataOffset);
							setInputLayout(vertexDecl, m_program[programIdx], draw.m_instanceDataStride/16);
						}
						else
						{
							deviceCtx->IASetVertexBuffers(1, 1, s_zero.m_buffer, s_zero.m_zero, s_zero.m_zero);
							setInputLayout(vertexDecl, m_program[programIdx], 0);
						}
					}
					else
					{
						deviceCtx->IASetVertexBuffers(0, 1, s_zero.m_buffer, s_zero.m_zero, s_zero.m_zero);
					}
				}

				if (currentState.m_indexBuffer.idx != draw.m_indexBuffer.idx)
				{
					currentState.m_indexBuffer = draw.m_indexBuffer;

					uint16_t handle = draw.m_indexBuffer.idx;
					if (invalidHandle != handle)
					{
						const IndexBufferD3D11& ib = m_indexBuffers[handle];
						deviceCtx->IASetIndexBuffer(ib.m_ptr
							, 0 == (ib.m_flags & BGFX_BUFFER_INDEX32) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT
							, 0
							);
					}
					else
					{
						deviceCtx->IASetIndexBuffer(NULL, DXGI_FORMAT_R16_UINT, 0);
					}
				}

				if (isValid(currentState.m_vertexBuffer) )
				{
					uint32_t numVertices = draw.m_numVertices;
					if (UINT32_MAX == numVertices)
					{
						const VertexBufferD3D11& vb = m_vertexBuffers[currentState.m_vertexBuffer.idx];
						uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
						const VertexDecl& vertexDecl = m_vertexDecls[decl];
						numVertices = vb.m_size/vertexDecl.m_stride;
					}

					uint32_t numIndices        = 0;
					uint32_t numPrimsSubmitted = 0;
					uint32_t numInstances      = 0;
					uint32_t numPrimsRendered  = 0;
					uint32_t numDrawIndirect   = 0;

					if (isValid(draw.m_indirectBuffer) )
					{
						const VertexBufferD3D11& vb = m_vertexBuffers[draw.m_indirectBuffer.idx];
						ID3D11Buffer* ptr = vb.m_ptr;

						if (isValid(draw.m_indexBuffer) )
						{
							numDrawIndirect = UINT16_MAX == draw.m_numIndirect
								? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
								: draw.m_numIndirect
								;

							multiDrawIndexedInstancedIndirect(numDrawIndirect
								, ptr
								, draw.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE
								, BGFX_CONFIG_DRAW_INDIRECT_STRIDE
								);
						}
						else
						{
							numDrawIndirect = UINT16_MAX == draw.m_numIndirect
								? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
								: draw.m_numIndirect
								;

							multiDrawInstancedIndirect(numDrawIndirect
								, ptr
								, draw.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE
								, BGFX_CONFIG_DRAW_INDIRECT_STRIDE
								);
						}
					}
					else
					{
						if (isValid(draw.m_indexBuffer) )
						{
							if (UINT32_MAX == draw.m_numIndices)
							{
								const IndexBufferD3D11& ib = m_indexBuffers[draw.m_indexBuffer.idx];
								const uint32_t indexSize = 0 == (ib.m_flags & BGFX_BUFFER_INDEX32) ? 2 : 4;
								numIndices        = ib.m_size/indexSize;
								numPrimsSubmitted = numIndices/prim.m_div - prim.m_sub;
								numInstances      = draw.m_numInstances;
								numPrimsRendered  = numPrimsSubmitted*draw.m_numInstances;

								if (numInstances > 1)
								{
									deviceCtx->DrawIndexedInstanced(numIndices
										, draw.m_numInstances
										, 0
										, draw.m_startVertex
										, 0
										);
								}
								else
								{
									deviceCtx->DrawIndexed(numIndices
										, 0
										, draw.m_startVertex
										);
								}
							}
							else if (prim.m_min <= draw.m_numIndices)
							{
								numIndices        = draw.m_numIndices;
								numPrimsSubmitted = numIndices/prim.m_div - prim.m_sub;
								numInstances      = draw.m_numInstances;
								numPrimsRendered  = numPrimsSubmitted*draw.m_numInstances;

								if (numInstances > 1)
								{
									deviceCtx->DrawIndexedInstanced(numIndices
										, draw.m_numInstances
										, draw.m_startIndex
										, draw.m_startVertex
										, 0
										);
								}
								else
								{
									deviceCtx->DrawIndexed(numIndices
										, draw.m_startIndex
										, draw.m_startVertex
										);
								}
							}
						}
						else
						{
							numPrimsSubmitted = numVertices/prim.m_div - prim.m_sub;
							numInstances      = draw.m_numInstances;
							numPrimsRendered  = numPrimsSubmitted*draw.m_numInstances;

							if (numInstances > 1)
							{
								deviceCtx->DrawInstanced(numVertices
									, draw.m_numInstances
									, draw.m_startVertex
									, 0
									);
							}
							else
							{
								deviceCtx->Draw(numVertices
									, draw.m_startVertex
									);
							}
						}
					}

					statsNumPrimsSubmitted[primIndex] += numPrimsSubmitted;
					statsNumPrimsRendered[primIndex]  += numPrimsRendered;
					statsNumInstances[primIndex]      += numInstances;
					statsNumDrawIndirect[primIndex]   += numDrawIndirect;
					statsNumIndices                   += numIndices;
				}
			}

			if (wasCompute)
			{
				if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
				{
					wchar_t* viewNameW = s_viewNameW[view];
					viewNameW[3] = L'C';
					PIX_ENDEVENT();
					PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), viewNameW);
				}

				invalidateCompute();
			}

			if (0 < _render->m_num)
			{
				if (0 != (m_resolution.m_flags & BGFX_RESET_FLUSH_AFTER_RENDER) )
				{
					deviceCtx->Flush();
				}

				captureElapsed = -bx::getHPCounter();
				capture();
				captureElapsed += bx::getHPCounter();
			}
		}

		PIX_ENDEVENT();

		int64_t now = bx::getHPCounter();
		elapsed += now;

		static int64_t last = now;
		int64_t frameTime = now - last;
		last = now;

		static int64_t min = frameTime;
		static int64_t max = frameTime;
		min = min > frameTime ? frameTime : min;
		max = max < frameTime ? frameTime : max;

		static uint32_t maxGpuLatency = 0;
		static double   maxGpuElapsed = 0.0f;
		double elapsedGpuMs = 0.0;

		m_gpuTimer.end();

		while (m_gpuTimer.get() )
		{
			double toGpuMs = 1000.0 / double(m_gpuTimer.m_frequency);
			elapsedGpuMs   = m_gpuTimer.m_elapsed * toGpuMs;
			maxGpuElapsed  = elapsedGpuMs > maxGpuElapsed ? elapsedGpuMs : maxGpuElapsed;
		}
		maxGpuLatency = bx::uint32_imax(maxGpuLatency, m_gpuTimer.m_control.available()-1);

		const int64_t timerFreq = bx::getHPFrequency();

		Stats& perfStats   = _render->m_perfStats;
		perfStats.cpuTime      = frameTime;
		perfStats.cpuTimerFreq = timerFreq;
		perfStats.gpuTime      = m_gpuTimer.m_elapsed;
		perfStats.gpuTimerFreq = m_gpuTimer.m_frequency;

		if (_render->m_debug & (BGFX_DEBUG_IFH|BGFX_DEBUG_STATS) )
		{
			PIX_BEGINEVENT(D3DCOLOR_RGBA(0x40, 0x40, 0x40, 0xff), L"debugstats");

			TextVideoMem& tvm = m_textVideoMem;

			static int64_t next = now;

			if (now >= next)
			{
				next = now + timerFreq;
				double freq = double(bx::getHPFrequency() );
				double toMs = 1000.0/freq;

				tvm.clear();
				uint16_t pos = 0;
				tvm.printf(0, pos++, BGFX_CONFIG_DEBUG ? 0x89 : 0x8f
					, " %s / " BX_COMPILER_NAME " / " BX_CPU_NAME " / " BX_ARCH_NAME " / " BX_PLATFORM_NAME " "
					, getRendererName()
					);

				const DXGI_ADAPTER_DESC& desc = m_adapterDesc;
				char description[BX_COUNTOF(desc.Description)];
				wcstombs(description, desc.Description, BX_COUNTOF(desc.Description) );
				tvm.printf(0, pos++, 0x8f, " Device: %s", description);

				char dedicatedVideo[16];
				bx::prettify(dedicatedVideo, BX_COUNTOF(dedicatedVideo), desc.DedicatedVideoMemory);

				char dedicatedSystem[16];
				bx::prettify(dedicatedSystem, BX_COUNTOF(dedicatedSystem), desc.DedicatedSystemMemory);

				char sharedSystem[16];
				bx::prettify(sharedSystem, BX_COUNTOF(sharedSystem), desc.SharedSystemMemory);

				tvm.printf(0, pos++, 0x8f, " Memory: %s (video), %s (system), %s (shared)"
					, dedicatedVideo
					, dedicatedSystem
					, sharedSystem
					);

				pos = 10;
				tvm.printf(10, pos++, 0x8e, "       Frame: %7.3f, % 7.3f \x1f, % 7.3f \x1e [ms] / % 6.2f FPS "
					, double(frameTime)*toMs
					, double(min)*toMs
					, double(max)*toMs
					, freq/frameTime
					);

				char hmd[16];
				bx::snprintf(hmd, BX_COUNTOF(hmd), ", [%c] HMD ", hmdEnabled ? '\xfe' : ' ');

				const uint32_t msaa = (m_resolution.m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT;
				tvm.printf(10, pos++, 0x8e, " Reset flags: [%c] vsync, [%c] MSAAx%d%s, [%c] MaxAnisotropy "
					, !!(m_resolution.m_flags&BGFX_RESET_VSYNC) ? '\xfe' : ' '
					, 0 != msaa ? '\xfe' : ' '
					, 1<<msaa
					, m_ovr.isInitialized() ? hmd : ", no-HMD "
					, !!(m_resolution.m_flags&BGFX_RESET_MAXANISOTROPY) ? '\xfe' : ' '
					);

				double elapsedCpuMs = double(elapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "   Submitted: %5d (draw %5d, compute %4d) / CPU %7.4f [ms] %c GPU %7.4f [ms] (latency %d) "
					, _render->m_num
					, statsKeyType[0]
					, statsKeyType[1]
					, elapsedCpuMs
					, elapsedCpuMs > maxGpuElapsed ? '>' : '<'
					, maxGpuElapsed
					, maxGpuLatency
					);
				maxGpuLatency = 0;
				maxGpuElapsed = 0.0;

				for (uint32_t ii = 0; ii < BX_COUNTOF(s_primName); ++ii)
				{
					tvm.printf(10, pos++, 0x8e, "   %9s: %7d (#inst: %5d), submitted: %7d, indirect %7d"
						, s_primName[ii]
						, statsNumPrimsRendered[ii]
						, statsNumInstances[ii]
						, statsNumPrimsSubmitted[ii]
						, statsNumDrawIndirect[ii]
						);
				}

				if (NULL != m_renderdocdll)
				{
					tvm.printf(tvm.m_width-27, 0, 0x1f, " [F11 - RenderDoc capture] ");
				}

				tvm.printf(10, pos++, 0x8e, "      Indices: %7d ", statsNumIndices);
				tvm.printf(10, pos++, 0x8e, " Uniform size: %7d ", _render->m_constEnd);
				tvm.printf(10, pos++, 0x8e, "     DVB size: %7d ", _render->m_vboffset);
				tvm.printf(10, pos++, 0x8e, "     DIB size: %7d ", _render->m_iboffset);

				pos++;
				tvm.printf(10, pos++, 0x8e, " State cache:                                ");
				tvm.printf(10, pos++, 0x8e, " Blend  | DepthS | Input  | Raster | Sampler ");
				tvm.printf(10, pos++, 0x8e, " %6d | %6d | %6d | %6d | %6d  "
					, m_blendStateCache.getCount()
					, m_depthStencilStateCache.getCount()
					, m_inputLayoutCache.getCount()
					, m_rasterizerStateCache.getCount()
					, m_samplerStateCache.getCount()
					);
				pos++;

				double captureMs = double(captureElapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "     Capture: %7.4f [ms] ", captureMs);

				uint8_t attr[2] = { 0x89, 0x8a };
				uint8_t attrIndex = _render->m_waitSubmit < _render->m_waitRender;

				tvm.printf(10, pos++, attr[attrIndex&1], " Submit wait: %7.4f [ms] ", _render->m_waitSubmit*toMs);
				tvm.printf(10, pos++, attr[(attrIndex+1)&1], " Render wait: %7.4f [ms] ", _render->m_waitRender*toMs);

				min = frameTime;
				max = frameTime;
			}

			blit(this, _textVideoMemBlitter, tvm);

			PIX_ENDEVENT();
		}
		else if (_render->m_debug & BGFX_DEBUG_TEXT)
		{
			PIX_BEGINEVENT(D3DCOLOR_RGBA(0x40, 0x40, 0x40, 0xff), L"debugtext");

			blit(this, _textVideoMemBlitter, _render->m_textVideoMem);

			PIX_ENDEVENT();
		}
	}
} /* namespace d3d11 */ } // namespace bgfx

#else

namespace bgfx { namespace d3d11
{
	RendererContextI* rendererCreate()
	{
		return NULL;
	}

	void rendererDestroy()
	{
	}
} /* namespace d3d11 */ } // namespace bgfx

#endif // BGFX_CONFIG_RENDERER_DIRECT3D11
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_DIRECT3D12
#	include "renderer_d3d12.h"

#	if !USE_D3D12_DYNAMIC_LIB
#		pragma comment(lib, "D3D12.lib")
#	endif // !USE_D3D12_DYNAMIC_LIB

namespace bgfx { namespace d3d12
{
	static wchar_t s_viewNameW[BGFX_CONFIG_MAX_VIEWS][256];

	struct PrimInfo
	{
		D3D_PRIMITIVE_TOPOLOGY m_toplogy;
		D3D12_PRIMITIVE_TOPOLOGY_TYPE m_topologyType;
		uint32_t m_min;
		uint32_t m_div;
		uint32_t m_sub;
	};

	static const PrimInfo s_primInfo[] =
	{
		{ D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,  3, 3, 0 },
		{ D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,  3, 1, 2 },
		{ D3D_PRIMITIVE_TOPOLOGY_LINELIST,      D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,      2, 2, 0 },
		{ D3D_PRIMITIVE_TOPOLOGY_POINTLIST,     D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,     1, 1, 0 },
		{ D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,     D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED, 0, 0, 0 },
	};

	static const char* s_primName[] =
	{
		"TriList",
		"TriStrip",
		"Line",
		"Point",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_primInfo) == BX_COUNTOF(s_primName)+1);

	static const uint32_t s_checkMsaa[] =
	{
		0,
		2,
		4,
		8,
		16,
	};

	static DXGI_SAMPLE_DESC s_msaa[] =
	{
		{  1, 0 },
		{  2, 0 },
		{  4, 0 },
		{  8, 0 },
		{ 16, 0 },
	};

	static const D3D12_BLEND s_blendFactor[][2] =
	{
		{ (D3D12_BLEND)0,               (D3D12_BLEND)0               }, // ignored
		{ D3D12_BLEND_ZERO,             D3D12_BLEND_ZERO             }, // ZERO
		{ D3D12_BLEND_ONE,              D3D12_BLEND_ONE              },	// ONE
		{ D3D12_BLEND_SRC_COLOR,        D3D12_BLEND_SRC_ALPHA        },	// SRC_COLOR
		{ D3D12_BLEND_INV_SRC_COLOR,    D3D12_BLEND_INV_SRC_ALPHA    },	// INV_SRC_COLOR
		{ D3D12_BLEND_SRC_ALPHA,        D3D12_BLEND_SRC_ALPHA        },	// SRC_ALPHA
		{ D3D12_BLEND_INV_SRC_ALPHA,    D3D12_BLEND_INV_SRC_ALPHA    },	// INV_SRC_ALPHA
		{ D3D12_BLEND_DEST_ALPHA,       D3D12_BLEND_DEST_ALPHA       },	// DST_ALPHA
		{ D3D12_BLEND_INV_DEST_ALPHA,   D3D12_BLEND_INV_DEST_ALPHA   },	// INV_DST_ALPHA
		{ D3D12_BLEND_DEST_COLOR,       D3D12_BLEND_DEST_ALPHA       },	// DST_COLOR
		{ D3D12_BLEND_INV_DEST_COLOR,   D3D12_BLEND_INV_DEST_ALPHA   },	// INV_DST_COLOR
		{ D3D12_BLEND_SRC_ALPHA_SAT,    D3D12_BLEND_ONE              },	// SRC_ALPHA_SAT
		{ D3D12_BLEND_BLEND_FACTOR,     D3D12_BLEND_BLEND_FACTOR     },	// FACTOR
		{ D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR },	// INV_FACTOR
	};

	static const D3D12_BLEND_OP s_blendEquation[] =
	{
		D3D12_BLEND_OP_ADD,
		D3D12_BLEND_OP_SUBTRACT,
		D3D12_BLEND_OP_REV_SUBTRACT,
		D3D12_BLEND_OP_MIN,
		D3D12_BLEND_OP_MAX,
	};

	static const D3D12_COMPARISON_FUNC s_cmpFunc[] =
	{
		D3D12_COMPARISON_FUNC(0), // ignored
		D3D12_COMPARISON_FUNC_LESS,
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_COMPARISON_FUNC_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER_EQUAL,
		D3D12_COMPARISON_FUNC_GREATER,
		D3D12_COMPARISON_FUNC_NOT_EQUAL,
		D3D12_COMPARISON_FUNC_NEVER,
		D3D12_COMPARISON_FUNC_ALWAYS,
	};

	static const D3D12_STENCIL_OP s_stencilOp[] =
	{
		D3D12_STENCIL_OP_ZERO,
		D3D12_STENCIL_OP_KEEP,
		D3D12_STENCIL_OP_REPLACE,
		D3D12_STENCIL_OP_INCR,
		D3D12_STENCIL_OP_INCR_SAT,
		D3D12_STENCIL_OP_DECR,
		D3D12_STENCIL_OP_DECR_SAT,
		D3D12_STENCIL_OP_INVERT,
	};

	static const D3D12_CULL_MODE s_cullMode[] =
	{
		D3D12_CULL_MODE_NONE,
		D3D12_CULL_MODE_FRONT,
		D3D12_CULL_MODE_BACK,
	};

	static const D3D12_TEXTURE_ADDRESS_MODE s_textureAddress[] =
	{
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,
		D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
	};

	/*
	 * D3D11_FILTER_MIN_MAG_MIP_POINT               = 0x00,
	 * D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR        = 0x01,
	 * D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT  = 0x04,
	 * D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR        = 0x05,
	 * D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT        = 0x10,
	 * D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
	 * D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT        = 0x14,
	 * D3D11_FILTER_MIN_MAG_MIP_LINEAR              = 0x15,
	 * D3D11_FILTER_ANISOTROPIC                     = 0x55,
	 *
	 * D3D11_COMPARISON_FILTERING_BIT               = 0x80,
	 * D3D11_ANISOTROPIC_FILTERING_BIT              = 0x40,
	 *
	 * According to D3D11_FILTER enum bits for mip, mag and mip are:
	 * 0x10 // MIN_LINEAR
	 * 0x04 // MAG_LINEAR
	 * 0x01 // MIP_LINEAR
	 */

	static const uint8_t s_textureFilter[3][3] =
	{
		{
			0x10, // min linear
			0x00, // min point
			0x55, // anisotropic
		},
		{
			0x04, // mag linear
			0x00, // mag point
			0x55, // anisotropic
		},
		{
			0x01, // mip linear
			0x00, // mip point
			0x55, // anisotropic
		},
	};

	struct TextureFormatInfo
	{
		DXGI_FORMAT m_fmt;
		DXGI_FORMAT m_fmtSrv;
		DXGI_FORMAT m_fmtDsv;
		DXGI_FORMAT m_fmtSrgb;
	};

	static const TextureFormatInfo s_textureFormat[] =
	{
		{ DXGI_FORMAT_BC1_UNORM,          DXGI_FORMAT_BC1_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC1_UNORM_SRGB      }, // BC1
		{ DXGI_FORMAT_BC2_UNORM,          DXGI_FORMAT_BC2_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC2_UNORM_SRGB      }, // BC2
		{ DXGI_FORMAT_BC3_UNORM,          DXGI_FORMAT_BC3_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC3_UNORM_SRGB      }, // BC3
		{ DXGI_FORMAT_BC4_UNORM,          DXGI_FORMAT_BC4_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // BC4
		{ DXGI_FORMAT_BC5_UNORM,          DXGI_FORMAT_BC5_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // BC5
		{ DXGI_FORMAT_BC6H_SF16,          DXGI_FORMAT_BC6H_SF16,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // BC6H
		{ DXGI_FORMAT_BC7_UNORM,          DXGI_FORMAT_BC7_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_BC7_UNORM_SRGB      }, // BC7
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC1
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC2
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC2A
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // ETC2A1
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC12
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC14
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC12A
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC14A
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC22
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // PTC24
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // Unknown
		{ DXGI_FORMAT_R1_UNORM,           DXGI_FORMAT_R1_UNORM,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R1
		{ DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8_UNORM,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8
		{ DXGI_FORMAT_R8_SINT,            DXGI_FORMAT_R8_SINT,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8I
		{ DXGI_FORMAT_R8_UINT,            DXGI_FORMAT_R8_UINT,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8U
		{ DXGI_FORMAT_R8_SNORM,           DXGI_FORMAT_R8_SNORM,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R8S
		{ DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_R16_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16
		{ DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SINT,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16I
		{ DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_R16_UNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16U
		{ DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16F
		{ DXGI_FORMAT_R16_SNORM,          DXGI_FORMAT_R16_SNORM,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R16S
		{ DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_UINT,              DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R32U
		{ DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R32F
		{ DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R8G8_UNORM,            DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8
		{ DXGI_FORMAT_R8G8_SINT,          DXGI_FORMAT_R8G8_SINT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8I
		{ DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_R8G8_UINT,             DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8U
		{ DXGI_FORMAT_R8G8_SNORM,         DXGI_FORMAT_R8G8_SNORM,            DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG8S
		{ DXGI_FORMAT_R16G16_UNORM,       DXGI_FORMAT_R16G16_UNORM,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16
		{ DXGI_FORMAT_R16G16_SINT,        DXGI_FORMAT_R16G16_SINT,           DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16I
		{ DXGI_FORMAT_R16G16_UINT,        DXGI_FORMAT_R16G16_UINT,           DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16U
		{ DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16F
		{ DXGI_FORMAT_R16G16_SNORM,       DXGI_FORMAT_R16G16_SNORM,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG16S
		{ DXGI_FORMAT_R32G32_UINT,        DXGI_FORMAT_R32G32_UINT,           DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG32U
		{ DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RG32F
		{ DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_B8G8R8A8_UNORM_SRGB }, // BGRA8
		{ DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }, // RGBA8
		{ DXGI_FORMAT_R8G8B8A8_SINT,      DXGI_FORMAT_R8G8B8A8_SINT,         DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }, // RGBA8I
		{ DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R8G8B8A8_UINT,         DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_R8G8B8A8_UNORM_SRGB }, // RGBA8U
		{ DXGI_FORMAT_R8G8B8A8_SNORM,     DXGI_FORMAT_R8G8B8A8_SNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA8S
		{ DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16
		{ DXGI_FORMAT_R16G16B16A16_SINT,  DXGI_FORMAT_R16G16B16A16_SINT,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16I
		{ DXGI_FORMAT_R16G16B16A16_UINT,  DXGI_FORMAT_R16G16B16A16_UINT,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16U
		{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16F
		{ DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA16S
		{ DXGI_FORMAT_R32G32B32A32_UINT,  DXGI_FORMAT_R32G32B32A32_UINT,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA32U
		{ DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,    DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA32F
		{ DXGI_FORMAT_B5G6R5_UNORM,       DXGI_FORMAT_B5G6R5_UNORM,          DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R5G6B5
		{ DXGI_FORMAT_B4G4R4A4_UNORM,     DXGI_FORMAT_B4G4R4A4_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGBA4
		{ DXGI_FORMAT_B5G5R5A1_UNORM,     DXGI_FORMAT_B5G5R5A1_UNORM,        DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGB5A1
		{ DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R10G10B10A2_UNORM,     DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // RGB10A2
		{ DXGI_FORMAT_R11G11B10_FLOAT,    DXGI_FORMAT_R11G11B10_FLOAT,       DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // R11G11B10F
		{ DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,               DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN             }, // UnknownDepth
		{ DXGI_FORMAT_R16_TYPELESS,       DXGI_FORMAT_R16_UNORM,             DXGI_FORMAT_D16_UNORM,         DXGI_FORMAT_UNKNOWN             }, // D16
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D24
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D24S8
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D32
		{ DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_D32_FLOAT,         DXGI_FORMAT_UNKNOWN             }, // D16F
		{ DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_D32_FLOAT,         DXGI_FORMAT_UNKNOWN             }, // D24F
		{ DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,             DXGI_FORMAT_D32_FLOAT,         DXGI_FORMAT_UNKNOWN             }, // D32F
		{ DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN             }, // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_textureFormat) );

	static const D3D12_INPUT_ELEMENT_DESC s_attrib[] =
	{
		{ "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",      0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BITANGENT",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",        0, DXGI_FORMAT_R8G8B8A8_UINT,   0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",        1, DXGI_FORMAT_R8G8B8A8_UINT,   0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,   0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     1, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     2, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     3, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     4, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     5, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     6, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD",     7, DXGI_FORMAT_R32G32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	BX_STATIC_ASSERT(Attrib::Count == BX_COUNTOF(s_attrib) );

	static const DXGI_FORMAT s_attribType[][4][2] =
	{
		{ // Uint8
			{ DXGI_FORMAT_R8_UINT,            DXGI_FORMAT_R8_UNORM           },
			{ DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_R8G8_UNORM         },
			{ DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R8G8B8A8_UNORM     },
			{ DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_R8G8B8A8_UNORM     },
		},
		{ // Uint10
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
			{ DXGI_FORMAT_R10G10B10A2_UINT,   DXGI_FORMAT_R10G10B10A2_UNORM  },
		},
		{ // Int16
			{ DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SNORM          },
			{ DXGI_FORMAT_R16G16_SINT,        DXGI_FORMAT_R16G16_SNORM       },
			{ DXGI_FORMAT_R16G16B16A16_SINT,  DXGI_FORMAT_R16G16B16A16_SNORM },
			{ DXGI_FORMAT_R16G16B16A16_SINT,  DXGI_FORMAT_R16G16B16A16_SNORM },
		},
		{ // Half
			{ DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT          },
			{ DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT       },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
			{ DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
		},
		{ // Float
			{ DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT          },
			{ DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT       },
			{ DXGI_FORMAT_R32G32B32_FLOAT,    DXGI_FORMAT_R32G32B32_FLOAT    },
			{ DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT },
		},
	};
	BX_STATIC_ASSERT(AttribType::Count == BX_COUNTOF(s_attribType) );

	static D3D12_INPUT_ELEMENT_DESC* fillVertexDecl(D3D12_INPUT_ELEMENT_DESC* _out, const VertexDecl& _decl)
	{
		D3D12_INPUT_ELEMENT_DESC* elem = _out;

		for (uint32_t attr = 0; attr < Attrib::Count; ++attr)
		{
			if (UINT16_MAX != _decl.m_attributes[attr])
			{
				memcpy(elem, &s_attrib[attr], sizeof(D3D12_INPUT_ELEMENT_DESC) );

				if (0 == _decl.m_attributes[attr])
				{
					elem->AlignedByteOffset = 0;
				}
				else
				{
					uint8_t num;
					AttribType::Enum type;
					bool normalized;
					bool asInt;
					_decl.decode(Attrib::Enum(attr), num, type, normalized, asInt);
					elem->Format = s_attribType[type][num-1][normalized];
					elem->AlignedByteOffset = _decl.m_offset[attr];
				}

				++elem;
			}
		}

		return elem;
	}

	void setResourceBarrier(ID3D12GraphicsCommandList* _commandList, ID3D12Resource* _resource, D3D12_RESOURCE_STATES _stateBefore, D3D12_RESOURCE_STATES _stateAfter)
	{
		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource   = _resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = _stateBefore;
		barrier.Transition.StateAfter  = _stateAfter;
		_commandList->ResourceBarrier(1, &barrier);
	}

	static const GUID IID_ID3D12CommandAllocator    = { 0x6102dee4, 0xaf59, 0x4b09, { 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24 } };
	static const GUID IID_ID3D12CommandQueue        = { 0x0ec870a6, 0x5d7e, 0x4c22, { 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed } };
	static const GUID IID_ID3D12CommandSignature    = { 0xc36a797c, 0xec80, 0x4f0a, { 0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1 } };
	static const GUID IID_ID3D12Debug               = { 0x344488b7, 0x6846, 0x474b, { 0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0 } };
	static const GUID IID_ID3D12DescriptorHeap      = { 0x8efb471d, 0x616c, 0x4f49, { 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51 } };
	static const GUID IID_ID3D12Device              = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
	static const GUID IID_ID3D12Fence               = { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };
	static const GUID IID_ID3D12GraphicsCommandList = { 0x5b160d0f, 0xac1b, 0x4185, { 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55 } };
	static const GUID IID_ID3D12InfoQueue           = { 0x0742a90b, 0xc387, 0x483f, { 0xb9, 0x46, 0x30, 0xa7, 0xe4, 0xe6, 0x14, 0x58 } };
	static const GUID IID_ID3D12PipelineState       = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };
	static const GUID IID_ID3D12Resource            = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
	static const GUID IID_ID3D12RootSignature       = { 0xc54a6b66, 0x72df, 0x4ee8, { 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14 } };
	static const GUID IID_IDXGIFactory4             = { 0x1bc6ea02, 0xef36, 0x464f, { 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a } };

	struct HeapProperty
	{
		enum Enum
		{
			Default,
			Upload,
			ReadBack,

			Count
		};

		D3D12_HEAP_PROPERTIES m_properties;
		D3D12_RESOURCE_STATES m_state;
	};

	static const HeapProperty s_heapProperties[] =
	{
		{ { D3D12_HEAP_TYPE_DEFAULT,  D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 }, D3D12_RESOURCE_STATE_COMMON       },
		{ { D3D12_HEAP_TYPE_UPLOAD,   D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 }, D3D12_RESOURCE_STATE_GENERIC_READ },
		{ { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 }, D3D12_RESOURCE_STATE_COPY_DEST    },
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_heapProperties) == HeapProperty::Count);

	ID3D12Resource* createCommittedResource(ID3D12Device* _device, HeapProperty::Enum _heapProperty, D3D12_RESOURCE_DESC* _resourceDesc, D3D12_CLEAR_VALUE* _clearValue)
	{
		const HeapProperty& heapProperty = s_heapProperties[_heapProperty];
		ID3D12Resource* resource;
		DX_CHECK(_device->CreateCommittedResource(&heapProperty.m_properties
				, D3D12_HEAP_FLAG_NONE
				, _resourceDesc
				, heapProperty.m_state
				, _clearValue
				, IID_ID3D12Resource
				, (void**)&resource
				) );
		BX_WARN(NULL != resource, "CreateCommittedResource failed (size: %d). Out of memory?"
			, _resourceDesc->Width
			);

		return resource;
	}

	ID3D12Resource* createCommittedResource(ID3D12Device* _device, HeapProperty::Enum _heapProperty, uint64_t _size, D3D12_RESOURCE_FLAGS _flags = D3D12_RESOURCE_FLAG_NONE)
	{
		D3D12_RESOURCE_DESC resourceDesc;
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Alignment = 0;
		resourceDesc.Width     = _size;
		resourceDesc.Height    = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format             = DXGI_FORMAT_UNKNOWN;
		resourceDesc.SampleDesc.Count   = 1;
		resourceDesc.SampleDesc.Quality = 0;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		resourceDesc.Flags  = _flags;

		return createCommittedResource(_device, _heapProperty, &resourceDesc, NULL);
	}

	BX_NO_INLINE void setDebugObjectName(ID3D12Object* _object, const char* _format, ...)
	{
		if (BX_ENABLED(BGFX_CONFIG_DEBUG_OBJECT_NAME) )
		{
			char temp[2048];
			va_list argList;
			va_start(argList, _format);
			int size = bx::uint32_min(sizeof(temp)-1, vsnprintf(temp, sizeof(temp), _format, argList) );
			va_end(argList);
			temp[size] = '\0';

			wchar_t* wtemp = (wchar_t*)alloca( (size+1)*2);
			mbstowcs(wtemp, temp, size+1);
			_object->SetName(wtemp);
		}
	}

#if USE_D3D12_DYNAMIC_LIB
	static PFN_D3D12_CREATE_DEVICE            D3D12CreateDevice;
	static PFN_D3D12_GET_DEBUG_INTERFACE      D3D12GetDebugInterface;
	static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE D3D12SerializeRootSignature;
	static PFN_CREATE_DXGI_FACTORY            CreateDXGIFactory1;

	typedef HANDLE  (WINAPI* PFN_CREATE_EVENT_EX_A)(LPSECURITY_ATTRIBUTES _attrs, LPCSTR _name, DWORD _flags, DWORD _access);
	static PFN_CREATE_EVENT_EX_A CreateEventExA;
#endif // USE_D3D12_DYNAMIC_LIB

	struct RendererContextD3D12 : public RendererContextI
	{
		RendererContextD3D12()
			: m_wireframe(false)
			, m_flags(BGFX_RESET_NONE)
			, m_fsChanges(0)
			, m_vsChanges(0)
			, m_backBufferColorIdx(0)
			, m_rtMsaa(false)
		{
		}

		~RendererContextD3D12()
		{
		}

		bool init()
		{
			struct ErrorState
			{
				enum Enum
				{
					Default,
					LoadedKernel32,
					LoadedD3D12,
					LoadedDXGI,
					CreatedDXGIFactory,
					CreatedCommandQueue,
				};
			};

			ErrorState::Enum errorState = ErrorState::Default;
			LUID luid;

			m_fbh.idx = invalidHandle;
			memset(m_uniforms, 0, sizeof(m_uniforms) );
			memset(&m_resolution, 0, sizeof(m_resolution) );

#if USE_D3D12_DYNAMIC_LIB
			m_kernel32dll = bx::dlopen("kernel32.dll");
			BX_WARN(NULL != m_kernel32dll, "Failed to load kernel32.dll.");
			if (NULL == m_kernel32dll)
			{
				goto error;
			}

			CreateEventExA = (PFN_CREATE_EVENT_EX_A)bx::dlsym(m_kernel32dll, "CreateEventExA");
			BX_WARN(NULL != CreateEventExA, "Function CreateEventExA not found.");
			if (NULL == CreateEventExA)
			{
				goto error;
			}

			errorState = ErrorState::LoadedKernel32;

			m_d3d12dll = bx::dlopen("d3d12.dll");
			BX_WARN(NULL != m_d3d12dll, "Failed to load d3d12.dll.");
			if (NULL == m_d3d12dll)
			{
				goto error;
			}

			errorState = ErrorState::LoadedD3D12;

			D3D12CreateDevice = (PFN_D3D12_CREATE_DEVICE)bx::dlsym(m_d3d12dll, "D3D12CreateDevice");
			BX_WARN(NULL != D3D12CreateDevice, "Function D3D12CreateDevice not found.");

			D3D12GetDebugInterface = (PFN_D3D12_GET_DEBUG_INTERFACE)bx::dlsym(m_d3d12dll, "D3D12GetDebugInterface");
			BX_WARN(NULL != D3D12GetDebugInterface, "Function D3D12GetDebugInterface not found.");

			D3D12SerializeRootSignature = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)bx::dlsym(m_d3d12dll, "D3D12SerializeRootSignature");
			BX_WARN(NULL != D3D12SerializeRootSignature, "Function D3D12SerializeRootSignature not found.");

			if (NULL == D3D12CreateDevice
			||  NULL == D3D12GetDebugInterface
			||  NULL == D3D12SerializeRootSignature)
			{
				goto error;
			}

			m_dxgidll = bx::dlopen("dxgi.dll");
			BX_WARN(NULL != m_dxgidll, "Failed to load dxgi.dll.");

			if (NULL == m_dxgidll)
			{
				goto error;
			}

			CreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY)bx::dlsym(m_dxgidll, "CreateDXGIFactory1");
			BX_WARN(NULL != CreateDXGIFactory1, "Function CreateDXGIFactory1 not found.");

			if (NULL == CreateDXGIFactory1)
			{
				goto error;
			}
#endif // USE_D3D12_DYNAMIC_LIB

			errorState = ErrorState::LoadedDXGI;

			HRESULT hr;

			hr = CreateDXGIFactory1(IID_IDXGIFactory4, (void**)&m_factory);
			BX_WARN(SUCCEEDED(hr), "Unable to create DXGI factory.");

			if (FAILED(hr) )
			{
				goto error;
			}

			errorState = ErrorState::CreatedDXGIFactory;

			m_adapter = NULL;
			m_driverType = D3D_DRIVER_TYPE_HARDWARE;

			{
				IDXGIAdapter3* adapter;
				for (uint32_t ii = 0; DXGI_ERROR_NOT_FOUND != m_factory->EnumAdapters(ii, reinterpret_cast<IDXGIAdapter**>(&adapter) ); ++ii)
				{
					DXGI_ADAPTER_DESC desc;
					hr = adapter->GetDesc(&desc);
					if (SUCCEEDED(hr) )
					{
						BX_TRACE("Adapter #%d", ii);

						char description[BX_COUNTOF(desc.Description)];
						wcstombs(description, desc.Description, BX_COUNTOF(desc.Description) );
						BX_TRACE("\tDescription: %s", description);
						BX_TRACE("\tVendorId: 0x%08x, DeviceId: 0x%08x, SubSysId: 0x%08x, Revision: 0x%08x"
								, desc.VendorId
								, desc.DeviceId
								, desc.SubSysId
								, desc.Revision
								);
						BX_TRACE("\tMemory: %" PRIi64 " (video), %" PRIi64 " (system), %" PRIi64 " (shared)"
								, desc.DedicatedVideoMemory
								, desc.DedicatedSystemMemory
								, desc.SharedSystemMemory
								);

						g_caps.gpu[ii].vendorId = (uint16_t)desc.VendorId;
						g_caps.gpu[ii].deviceId = (uint16_t)desc.DeviceId;
						++g_caps.numGPUs;

						if ( (BGFX_PCI_ID_NONE != g_caps.vendorId ||             0 != g_caps.deviceId)
						&&   (BGFX_PCI_ID_NONE == g_caps.vendorId || desc.VendorId == g_caps.vendorId)
						&&   (0 == g_caps.deviceId                || desc.DeviceId == g_caps.deviceId) )
						{
							m_adapter = adapter;
							m_adapter->AddRef();
							m_driverType = D3D_DRIVER_TYPE_UNKNOWN;
						}

						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PERFHUD)
						&&  0 != strstr(description, "PerfHUD") )
						{
							m_adapter = adapter;
							m_driverType = D3D_DRIVER_TYPE_REFERENCE;
						}
					}

					DX_RELEASE(adapter, adapter == m_adapter ? 1 : 0);
				}
			}

			if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
			{
				ID3D12Debug* debug;
				hr = D3D12GetDebugInterface(IID_ID3D12Debug, (void**)&debug);

				if (SUCCEEDED(hr) )
				{
					debug->EnableDebugLayer();
				}
			}

			{
				D3D_FEATURE_LEVEL featureLevel[] =
				{
					D3D_FEATURE_LEVEL_12_1,
					D3D_FEATURE_LEVEL_12_0,
					D3D_FEATURE_LEVEL_11_1,
					D3D_FEATURE_LEVEL_11_0,
				};

				hr = E_FAIL;
				for (uint32_t ii = 0; ii < BX_COUNTOF(featureLevel) && FAILED(hr); ++ii)
				{
					hr = D3D12CreateDevice(m_adapter
							, featureLevel[ii]
							, IID_ID3D12Device
							, (void**)&m_device
							);
					BX_WARN(FAILED(hr), "Direct3D12 device feature level %d.%d."
						, (featureLevel[ii] >> 12) & 0xf
						, (featureLevel[ii] >>  8) & 0xf
						);
				}
				BX_WARN(SUCCEEDED(hr), "Unable to create Direct3D12 device.");
			}

			if (FAILED(hr) )
			{
				goto error;
			}

			{
				memset(&m_adapterDesc, 0, sizeof(m_adapterDesc) );
				luid = m_device->GetAdapterLuid();
				IDXGIAdapter3* adapter;
				for (uint32_t ii = 0; DXGI_ERROR_NOT_FOUND != m_factory->EnumAdapters(ii, reinterpret_cast<IDXGIAdapter**>(&adapter) ); ++ii)
				{
					adapter->GetDesc(&m_adapterDesc);
					if (m_adapterDesc.AdapterLuid.LowPart  == luid.LowPart
					&&  m_adapterDesc.AdapterLuid.HighPart == luid.HighPart)
					{
						if (NULL == m_adapter)
						{
							m_adapter = adapter;
						}
						else
						{
							DX_RELEASE(adapter, 0);
						}
						break;
					}
					DX_RELEASE(adapter, 0);
				}
			}

			g_caps.vendorId = (uint16_t)m_adapterDesc.VendorId;
			g_caps.deviceId = (uint16_t)m_adapterDesc.DeviceId;

			{
				uint32_t numNodes = m_device->GetNodeCount();
				BX_TRACE("D3D12 GPU Architecture (num nodes: %d):", numNodes);
				for (uint32_t ii = 0; ii < numNodes; ++ii)
				{
					D3D12_FEATURE_DATA_ARCHITECTURE architecture;
					architecture.NodeIndex = ii;
					DX_CHECK(m_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture) ) );
					BX_TRACE("\tNode % 2d: TileBasedRenderer %d, UMA %d, CacheCoherentUMA %d"
							, ii
							, architecture.TileBasedRenderer
							, architecture.UMA
							, architecture.CacheCoherentUMA
							);
					if (0 == ii)
					{
						memcpy(&m_architecture, &architecture, sizeof(architecture) );
					}
				}
			}

			DX_CHECK(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &m_options, sizeof(m_options) ) );
			BX_TRACE("D3D12 options:")
			BX_TRACE("\tTiledResourcesTier %d", m_options.TiledResourcesTier);
			BX_TRACE("\tResourceBindingTier %d", m_options.ResourceBindingTier);
			BX_TRACE("\tROVsSupported %d", m_options.ROVsSupported);
			BX_TRACE("\tConservativeRasterizationTier %d", m_options.ConservativeRasterizationTier);
			BX_TRACE("\tCrossNodeSharingTier %d", m_options.CrossNodeSharingTier);
			BX_TRACE("\tResourceHeapTier %d", m_options.ResourceHeapTier);

			m_cmd.init(m_device);
			errorState = ErrorState::CreatedCommandQueue;

			m_scd.BufferDesc.Width  = BGFX_DEFAULT_WIDTH;
			m_scd.BufferDesc.Height = BGFX_DEFAULT_HEIGHT;
			m_scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			m_scd.BufferDesc.Scaling                 = DXGI_MODE_SCALING_STRETCHED;
			m_scd.BufferDesc.ScanlineOrdering        = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
			m_scd.BufferDesc.RefreshRate.Numerator   = 60;
			m_scd.BufferDesc.RefreshRate.Denominator = 1;
			m_scd.SampleDesc.Count   = 1;
			m_scd.SampleDesc.Quality = 0;
			m_scd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			m_scd.BufferCount  = bx::uint32_min(BX_COUNTOF(m_backBufferColor), 4);
			m_scd.OutputWindow = (HWND)g_platformData.nwh;
			m_scd.Windowed     = true;
			m_scd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
			m_scd.Flags        = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

			BX_CHECK(m_scd.BufferCount <= BX_COUNTOF(m_backBufferColor), "Swap chain buffer count %d (max %d)."
					, m_scd.BufferCount
					, BX_COUNTOF(m_backBufferColor)
					);
			hr = m_factory->CreateSwapChain(m_cmd.m_commandQueue
					, &m_scd
					, reinterpret_cast<IDXGISwapChain**>(&m_swapChain)
					);
			BX_WARN(SUCCEEDED(hr), "Failed to create swap chain.");
			if (FAILED(hr) )
			{
				goto error;
			}

			m_presentElapsed = 0;

			{
				m_resolution.m_width  = BGFX_DEFAULT_WIDTH;
				m_resolution.m_height = BGFX_DEFAULT_HEIGHT;

				DX_CHECK(m_factory->MakeWindowAssociation( (HWND)g_platformData.nwh
						, 0
						| DXGI_MWA_NO_WINDOW_CHANGES
						| DXGI_MWA_NO_ALT_ENTER
						) );

				m_numWindows = 1;

				if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
				{
					hr = m_device->QueryInterface(IID_ID3D12InfoQueue, (void**)&m_infoQueue);

					if (SUCCEEDED(hr) )
					{
						m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
						m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      true);
						m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,    false);

						D3D12_INFO_QUEUE_FILTER filter;
						memset(&filter, 0, sizeof(filter) );

						D3D12_MESSAGE_CATEGORY catlist[] =
						{
							D3D12_MESSAGE_CATEGORY_STATE_CREATION,
							D3D12_MESSAGE_CATEGORY_EXECUTION,
						};
						filter.DenyList.NumCategories = BX_COUNTOF(catlist);
						filter.DenyList.pCategoryList = catlist;
						m_infoQueue->PushStorageFilter(&filter);

						DX_RELEASE_WARNONLY(m_infoQueue, 0);
					}
				}

				D3D12_DESCRIPTOR_HEAP_DESC rtvDescHeap;
				rtvDescHeap.NumDescriptors = 0
						+ BX_COUNTOF(m_backBufferColor)
						+ BGFX_CONFIG_MAX_FRAME_BUFFERS*BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS
						;
				rtvDescHeap.Type     = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				rtvDescHeap.Flags    = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
				rtvDescHeap.NodeMask = 1;
				DX_CHECK(m_device->CreateDescriptorHeap(&rtvDescHeap
						, IID_ID3D12DescriptorHeap
						, (void**)&m_rtvDescriptorHeap
						) );

				D3D12_DESCRIPTOR_HEAP_DESC dsvDescHeap;
				dsvDescHeap.NumDescriptors = 0
						+ 1 // reserved for depth backbuffer.
						+ BGFX_CONFIG_MAX_FRAME_BUFFERS
						;
				dsvDescHeap.Type     = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
				dsvDescHeap.Flags    = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
				dsvDescHeap.NodeMask = 1;
				DX_CHECK(m_device->CreateDescriptorHeap(&dsvDescHeap
						, IID_ID3D12DescriptorHeap
						, (void**)&m_dsvDescriptorHeap
						) );

				for (uint32_t ii = 0; ii < BX_COUNTOF(m_scratchBuffer); ++ii)
				{
					m_scratchBuffer[ii].create(BGFX_CONFIG_MAX_DRAW_CALLS*1024
							, BGFX_CONFIG_MAX_TEXTURES + BGFX_CONFIG_MAX_SHADERS + BGFX_CONFIG_MAX_DRAW_CALLS
							);
				}
				m_samplerAllocator.create(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
						, 1024
						, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS
						);

				D3D12_DESCRIPTOR_RANGE descRange[] =
				{
					{ D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
					{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV,     BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
					{ D3D12_DESCRIPTOR_RANGE_TYPE_CBV,     1,                                0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
					{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV,     BGFX_CONFIG_MAX_TEXTURE_SAMPLERS, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
				};
				BX_STATIC_ASSERT(BX_COUNTOF(descRange) == Rdt::Count);

				D3D12_ROOT_PARAMETER rootParameter[] =
				{
					{ D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { 1, &descRange[Rdt::Sampler] }, D3D12_SHADER_VISIBILITY_ALL },
					{ D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { 1, &descRange[Rdt::SRV]     }, D3D12_SHADER_VISIBILITY_ALL },
					{ D3D12_ROOT_PARAMETER_TYPE_CBV,              { 0, 0                        }, D3D12_SHADER_VISIBILITY_ALL },
					{ D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, { 1, &descRange[Rdt::UAV]     }, D3D12_SHADER_VISIBILITY_ALL },
				};
				rootParameter[Rdt::CBV].Descriptor.RegisterSpace  = 0;
				rootParameter[Rdt::CBV].Descriptor.ShaderRegister = 0;

				D3D12_ROOT_SIGNATURE_DESC descRootSignature;
				descRootSignature.NumParameters = BX_COUNTOF(rootParameter);
				descRootSignature.pParameters   = rootParameter;
				descRootSignature.NumStaticSamplers = 0;
				descRootSignature.pStaticSamplers   = NULL;
				descRootSignature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

				ID3DBlob* outBlob;
				ID3DBlob* errorBlob;
				DX_CHECK(D3D12SerializeRootSignature(&descRootSignature
						, D3D_ROOT_SIGNATURE_VERSION_1
						, &outBlob
						, &errorBlob
						) );

				DX_CHECK(m_device->CreateRootSignature(0
						, outBlob->GetBufferPointer()
						, outBlob->GetBufferSize()
						, IID_ID3D12RootSignature
						, (void**)&m_rootSignature
						) );

				UniformHandle handle = BGFX_INVALID_HANDLE;
				for (uint32_t ii = 0; ii < PredefinedUniform::Count; ++ii)
				{
					m_uniformReg.add(handle, getPredefinedUniformName(PredefinedUniform::Enum(ii) ), &m_predefinedUniforms[ii]);
				}

				g_caps.supported |= ( 0
									| BGFX_CAPS_TEXTURE_3D
									| BGFX_CAPS_TEXTURE_COMPARE_ALL
									| BGFX_CAPS_INSTANCING
									| BGFX_CAPS_VERTEX_ATTRIB_HALF
									| BGFX_CAPS_VERTEX_ATTRIB_UINT10
									| BGFX_CAPS_FRAGMENT_DEPTH
									| BGFX_CAPS_BLEND_INDEPENDENT
									| BGFX_CAPS_COMPUTE
									| (m_options.ROVsSupported ? BGFX_CAPS_FRAGMENT_ORDERING : 0)
//									| BGFX_CAPS_SWAP_CHAIN
									);
				g_caps.maxTextureSize   = 16384;
				g_caps.maxFBAttachments = uint8_t(bx::uint32_min(16, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) );

				for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
				{
					uint8_t support = BGFX_CAPS_FORMAT_TEXTURE_NONE;

					const DXGI_FORMAT fmt = isDepth(TextureFormat::Enum(ii) )
						? s_textureFormat[ii].m_fmtDsv
						: s_textureFormat[ii].m_fmt
						;
					const DXGI_FORMAT fmtSrgb = s_textureFormat[ii].m_fmtSrgb;

					if (DXGI_FORMAT_UNKNOWN != fmt)
					{
						D3D12_FEATURE_DATA_FORMAT_SUPPORT data;
						data.Format = fmt;
						hr = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data) );
						if (SUCCEEDED(hr) )
						{
							support |= 0 != (data.Support1 & (0
									| D3D12_FORMAT_SUPPORT1_TEXTURE2D
									| D3D12_FORMAT_SUPPORT1_TEXTURE3D
									| D3D12_FORMAT_SUPPORT1_TEXTURECUBE
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_COLOR
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;

							support |= 0 != (data.Support1 & (0
									| D3D12_FORMAT_SUPPORT1_BUFFER
									| D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER
									| D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_VERTEX
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;

							support |= 0 != (data.Support1 & (0
									| D3D12_FORMAT_SUPPORT1_SHADER_LOAD
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_IMAGE
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;

							support |= 0 != (data.Support1 & (0
									| D3D12_FORMAT_SUPPORT1_RENDER_TARGET
									| D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;
						}
						else
						{
							BX_TRACE("CheckFeatureSupport failed with %x for format %s.", hr, getName(TextureFormat::Enum(ii) ) );
						}

						if (0 != (support & BGFX_CAPS_FORMAT_TEXTURE_IMAGE) )
						{
							// clear image flag for additional testing
							support &= ~BGFX_CAPS_FORMAT_TEXTURE_IMAGE;

							data.Format = s_textureFormat[ii].m_fmt;
							hr = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data) );
							if (SUCCEEDED(hr) )
							{
								support |= 0 != (data.Support2 & (0
										| D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD
										| D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE
										) )
										? BGFX_CAPS_FORMAT_TEXTURE_IMAGE
										: BGFX_CAPS_FORMAT_TEXTURE_NONE
										;
							}
						}
					}

					if (DXGI_FORMAT_UNKNOWN != fmtSrgb)
					{
						struct D3D11_FEATURE_DATA_FORMAT_SUPPORT
						{
							DXGI_FORMAT InFormat;
							UINT OutFormatSupport;
						};

						D3D12_FEATURE_DATA_FORMAT_SUPPORT data;
						data.Format = fmtSrgb;
						hr = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data) );
						if (SUCCEEDED(hr) )
						{
							support |= 0 != (data.Support1 & (0
									| D3D12_FORMAT_SUPPORT1_TEXTURE2D
									| D3D12_FORMAT_SUPPORT1_TEXTURE3D
									| D3D12_FORMAT_SUPPORT1_TEXTURECUBE
									) )
									? BGFX_CAPS_FORMAT_TEXTURE_COLOR_SRGB
									: BGFX_CAPS_FORMAT_TEXTURE_NONE
									;
						}
						else
						{
							BX_TRACE("CheckFeatureSupport failed with %x for sRGB format %s.", hr, getName(TextureFormat::Enum(ii) ) );
						}
					}

					g_caps.formats[ii] = support;
				}

				postReset();

				m_batch.create(4<<10);
			}
			return true;

		error:
			switch (errorState)
			{
			case ErrorState::CreatedCommandQueue:
				m_cmd.shutdown();
			case ErrorState::CreatedDXGIFactory:
				DX_RELEASE(m_device,  0);
				DX_RELEASE(m_adapter, 0);
				DX_RELEASE(m_factory, 0);
#if USE_D3D12_DYNAMIC_LIB
			case ErrorState::LoadedDXGI:
				bx::dlclose(m_dxgidll);
			case ErrorState::LoadedD3D12:
				bx::dlclose(m_d3d12dll);
			case ErrorState::LoadedKernel32:
				bx::dlclose(m_kernel32dll);
#endif // USE_D3D12_DYNAMIC_LIB
			case ErrorState::Default:
				break;
			}

			return false;
		}

		void shutdown()
		{
			m_batch.destroy();

			preReset();

			m_samplerAllocator.destroy();

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_scratchBuffer); ++ii)
			{
				m_scratchBuffer[ii].destroy();
			}

			m_pipelineStateCache.invalidate();

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_indexBuffers); ++ii)
			{
				m_indexBuffers[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_vertexBuffers); ++ii)
			{
				m_vertexBuffers[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_shaders); ++ii)
			{
				m_shaders[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_textures); ++ii)
			{
				m_textures[ii].destroy();
			}

			DX_RELEASE(m_rtvDescriptorHeap, 0);
			DX_RELEASE(m_dsvDescriptorHeap, 0);

			DX_RELEASE(m_rootSignature, 0);

			DX_RELEASE(m_swapChain, 0);

			m_cmd.shutdown();

			DX_RELEASE(m_device, 0);
			DX_RELEASE(m_adapter, 0);
			DX_RELEASE(m_factory, 0);

#if USE_D3D12_DYNAMIC_LIB
			bx::dlclose(m_dxgidll);
			bx::dlclose(m_d3d12dll);
			bx::dlclose(m_kernel32dll);
#endif // USE_D3D12_DYNAMIC_LIB
		}

		RendererType::Enum getRendererType() const BX_OVERRIDE
		{
			return RendererType::Direct3D12;
		}

		const char* getRendererName() const BX_OVERRIDE
		{
			return BGFX_RENDERER_DIRECT3D12_NAME;
		}

		static bool isLost(HRESULT _hr)
		{
			return DXGI_ERROR_DEVICE_REMOVED == _hr
				|| DXGI_ERROR_DEVICE_HUNG == _hr
				|| DXGI_ERROR_DEVICE_RESET == _hr
				|| DXGI_ERROR_DRIVER_INTERNAL_ERROR == _hr
				|| DXGI_ERROR_NOT_CURRENTLY_AVAILABLE == _hr
				;
		}

		void flip(HMD& /*_hmd*/) BX_OVERRIDE
		{
			if (NULL != m_swapChain)
			{
				int64_t start = bx::getHPCounter();

				HRESULT hr = 0;
				uint32_t syncInterval = !!(m_flags & BGFX_RESET_VSYNC);
				for (uint32_t ii = 1, num = m_numWindows; ii < num && SUCCEEDED(hr); ++ii)
				{
					hr = m_frameBuffers[m_windows[ii].idx].m_swapChain->Present(syncInterval, 0);
				}

				if (SUCCEEDED(hr) )
				{
					m_cmd.finish(m_backBufferColorFence[(m_backBufferColorIdx-1) % m_scd.BufferCount]);
					hr = m_swapChain->Present(syncInterval, 0);
				}

				int64_t now = bx::getHPCounter();
				m_presentElapsed = now - start;

				if (FAILED(hr)
				&&  isLost(hr) )
				{
					++m_lost;
					BGFX_FATAL(10 > m_lost, bgfx::Fatal::DeviceLost, "Device is lost. FAILED 0x%08x", hr);
				}
				else
				{
					m_lost = 0;
				}
			}
		}

		void createIndexBuffer(IndexBufferHandle _handle, Memory* _mem, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_mem->size, _mem->data, _flags, false);
		}

		void destroyIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createVertexDecl(VertexDeclHandle _handle, const VertexDecl& _decl) BX_OVERRIDE
		{
			VertexDecl& decl = m_vertexDecls[_handle.idx];
			memcpy(&decl, &_decl, sizeof(VertexDecl) );
			dump(decl);
		}

		void destroyVertexDecl(VertexDeclHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createVertexBuffer(VertexBufferHandle _handle, Memory* _mem, VertexDeclHandle _declHandle, uint16_t _flags) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].create(_mem->size, _mem->data, _declHandle, _flags);
		}

		void destroyVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_size, NULL, _flags, false);
		}

		void updateDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].update(m_commandList, _offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			VertexDeclHandle decl = BGFX_INVALID_HANDLE;
			m_vertexBuffers[_handle.idx].create(_size, NULL, decl, _flags);
		}

		void updateDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].update(m_commandList, _offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createShader(ShaderHandle _handle, Memory* _mem) BX_OVERRIDE
		{
			m_shaders[_handle.idx].create(_mem);
		}

		void destroyShader(ShaderHandle _handle) BX_OVERRIDE
		{
			m_shaders[_handle.idx].destroy();
		}

		void createProgram(ProgramHandle _handle, ShaderHandle _vsh, ShaderHandle _fsh) BX_OVERRIDE
		{
			m_program[_handle.idx].create(&m_shaders[_vsh.idx], isValid(_fsh) ? &m_shaders[_fsh.idx] : NULL);
		}

		void destroyProgram(ProgramHandle _handle) BX_OVERRIDE
		{
			m_program[_handle.idx].destroy();
		}

		void createTexture(TextureHandle _handle, Memory* _mem, uint32_t _flags, uint8_t _skip) BX_OVERRIDE
		{
			m_textures[_handle.idx].create(_mem, _flags, _skip);
		}

		void updateTextureBegin(TextureHandle /*_handle*/, uint8_t /*_side*/, uint8_t /*_mip*/) BX_OVERRIDE
		{
		}

		void updateTexture(TextureHandle _handle, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem) BX_OVERRIDE
		{
			m_textures[_handle.idx].update(m_commandList, _side, _mip, _rect, _z, _depth, _pitch, _mem);
		}

		void updateTextureEnd() BX_OVERRIDE
		{
		}

		void resizeTexture(TextureHandle _handle, uint16_t _width, uint16_t _height) BX_OVERRIDE
		{
			TextureD3D12& texture = m_textures[_handle.idx];

			uint32_t size = sizeof(uint32_t) + sizeof(TextureCreate);
			const Memory* mem = alloc(size);

			bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
			uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
			bx::write(&writer, magic);

			TextureCreate tc;
			tc.m_flags   = texture.m_flags;
			tc.m_width   = _width;
			tc.m_height  = _height;
			tc.m_sides   = 0;
			tc.m_depth   = 0;
			tc.m_numMips = 1;
			tc.m_format  = texture.m_requestedFormat;
			tc.m_cubeMap = false;
			tc.m_mem     = NULL;
			bx::write(&writer, tc);

			texture.destroy();
			texture.create(mem, tc.m_flags, 0);

			release(mem);
		}

		void destroyTexture(TextureHandle _handle) BX_OVERRIDE
		{
			m_textures[_handle.idx].destroy();
		}

		void createFrameBuffer(FrameBufferHandle _handle, uint8_t _num, const TextureHandle* _textureHandles) BX_OVERRIDE
		{
			m_frameBuffers[_handle.idx].create(_num, _textureHandles);
		}

		void createFrameBuffer(FrameBufferHandle _handle, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat) BX_OVERRIDE
		{
			uint16_t denseIdx = m_numWindows++;
			m_windows[denseIdx] = _handle;
			m_frameBuffers[_handle.idx].create(denseIdx, _nwh, _width, _height, _depthFormat);
		}

		void destroyFrameBuffer(FrameBufferHandle _handle) BX_OVERRIDE
		{
			uint16_t denseIdx = m_frameBuffers[_handle.idx].destroy();
			if (UINT16_MAX != denseIdx)
			{
				--m_numWindows;
				if (m_numWindows > 1)
				{
					FrameBufferHandle handle = m_windows[m_numWindows];
					m_windows[denseIdx] = handle;
					m_frameBuffers[handle.idx].m_denseIdx = denseIdx;
				}
			}
		}

		void createUniform(UniformHandle _handle, UniformType::Enum _type, uint16_t _num, const char* _name) BX_OVERRIDE
		{
			if (NULL != m_uniforms[_handle.idx])
			{
				BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			}

			uint32_t size = BX_ALIGN_16(g_uniformTypeSize[_type] * _num);
			void* data = BX_ALLOC(g_allocator, size);
			memset(data, 0, size);
			m_uniforms[_handle.idx] = data;
			m_uniformReg.add(_handle, _name, data);
		}

		void destroyUniform(UniformHandle _handle) BX_OVERRIDE
		{
			BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			m_uniforms[_handle.idx] = NULL;
		}

		void saveScreenShot(const char* _filePath) BX_OVERRIDE
		{
			uint32_t idx = (m_backBufferColorIdx-1) % m_scd.BufferCount;
			m_cmd.finish(m_backBufferColorFence[idx]);
			ID3D12Resource* backBuffer = m_backBufferColor[idx];

			D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();

			const uint32_t width  = (uint32_t)desc.Width;
			const uint32_t height = (uint32_t)desc.Height;

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
			uint32_t numRows;
			uint64_t total;
			uint64_t pitch;
			m_device->GetCopyableFootprints(&desc
				, 0
				, 1
				, 0
				, &layout
				, &numRows
				, &pitch
				, &total
				);

			ID3D12Resource* readback = createCommittedResource(m_device, HeapProperty::ReadBack, total);

			D3D12_BOX box;
			box.left   = 0;
			box.top    = 0;
			box.right  = width;
			box.bottom = height;
			box.front  = 0;
			box.back   = 1;

			setResourceBarrier(m_commandList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION dst = { readback,   D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,  layout };
			D3D12_TEXTURE_COPY_LOCATION src = { backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}     };
			m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			setResourceBarrier(m_commandList, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
			finish();
			m_commandList = m_cmd.alloc();

			void* data;
			readback->Map(0, NULL, (void**)&data);
			imageSwizzleBgra8(width
				, height
				, (uint32_t)pitch
				, data
				, data
				);
			g_callback->screenShot(_filePath
				, width
				, height
				, (uint32_t)pitch
				, data
				, (uint32_t)total
				, false
				);
			readback->Unmap(0, NULL);

			DX_RELEASE(readback, 0);
		}

		void updateViewName(uint8_t /*_id*/, const char* /*_name*/) BX_OVERRIDE
		{
		}

		void updateUniform(uint16_t _loc, const void* _data, uint32_t _size) BX_OVERRIDE
		{
			memcpy(m_uniforms[_loc], _data, _size);
		}

		void setMarker(const char* /*_marker*/, uint32_t /*_size*/) BX_OVERRIDE
		{
		}

		void submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter) BX_OVERRIDE;

		void blitSetup(TextVideoMemBlitter& _blitter) BX_OVERRIDE
		{
			const uint32_t width  = m_scd.BufferDesc.Width;
			const uint32_t height = m_scd.BufferDesc.Height;

			FrameBufferHandle fbh = BGFX_INVALID_HANDLE;
			setFrameBuffer(fbh, false);

			D3D12_VIEWPORT vp;
			vp.TopLeftX = 0;
			vp.TopLeftY = 0;
			vp.Width    = (float)width;
			vp.Height   = (float)height;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			m_commandList->RSSetViewports(1, &vp);

			D3D12_RECT rc;
			rc.left   = 0;
			rc.top    = 0;
			rc.right  = width;
			rc.bottom = height;
			m_commandList->RSSetScissorRects(1, &rc);

			const uint64_t state = 0
				| BGFX_STATE_RGB_WRITE
				| BGFX_STATE_ALPHA_WRITE
				| BGFX_STATE_DEPTH_TEST_ALWAYS
				;

			ID3D12PipelineState* pso = getPipelineState(state
				, packStencil(BGFX_STENCIL_DEFAULT, BGFX_STENCIL_DEFAULT)
				, _blitter.m_vb->decl.idx
				, _blitter.m_program.idx
				, 0
				);
			m_commandList->SetPipelineState(pso);
			m_commandList->SetGraphicsRootSignature(m_rootSignature);

			float proj[16];
			bx::mtxOrtho(proj, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1000.0f);

			PredefinedUniform& predefined = m_program[_blitter.m_program.idx].m_predefined[0];
			uint8_t flags = predefined.m_type;
			setShaderUniform(flags, predefined.m_loc, proj, 4);

			D3D12_GPU_VIRTUAL_ADDRESS gpuAddress;
			commitShaderConstants(_blitter.m_program.idx, gpuAddress);

			ScratchBufferD3D12& scratchBuffer = m_scratchBuffer[m_backBufferColorIdx];
			ID3D12DescriptorHeap* heaps[] =
			{
				m_samplerAllocator.getHeap(),
				scratchBuffer.getHeap(),
			};
			m_commandList->SetDescriptorHeaps(BX_COUNTOF(heaps), heaps);
			m_commandList->SetGraphicsRootConstantBufferView(Rdt::CBV, gpuAddress);

			TextureD3D12& texture = m_textures[_blitter.m_texture.idx];
			uint32_t samplerFlags[BGFX_CONFIG_MAX_TEXTURE_SAMPLERS] = { texture.m_flags & BGFX_TEXTURE_SAMPLER_BITS_MASK };
			uint16_t samplerStateIdx = getSamplerState(samplerFlags);
			m_commandList->SetGraphicsRootDescriptorTable(Rdt::Sampler, m_samplerAllocator.get(samplerStateIdx) );
			D3D12_GPU_DESCRIPTOR_HANDLE srvHandle;
			scratchBuffer.allocSrv(srvHandle, texture);
			m_commandList->SetGraphicsRootDescriptorTable(Rdt::SRV, srvHandle);

			VertexBufferD3D12& vb  = m_vertexBuffers[_blitter.m_vb->handle.idx];
			const VertexDecl& vertexDecl = m_vertexDecls[_blitter.m_vb->decl.idx];
			D3D12_VERTEX_BUFFER_VIEW viewDesc;
			viewDesc.BufferLocation = vb.m_gpuVA;
			viewDesc.StrideInBytes  = vertexDecl.m_stride;
			viewDesc.SizeInBytes    = vb.m_size;
			m_commandList->IASetVertexBuffers(0, 1, &viewDesc);

			const BufferD3D12& ib = m_indexBuffers[_blitter.m_ib->handle.idx];
			D3D12_INDEX_BUFFER_VIEW ibv;
			ibv.Format         = DXGI_FORMAT_R16_UINT;
			ibv.BufferLocation = ib.m_gpuVA;
			ibv.SizeInBytes    = ib.m_size;
			m_commandList->IASetIndexBuffer(&ibv);

			m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}

		void blitRender(TextVideoMemBlitter& _blitter, uint32_t _numIndices) BX_OVERRIDE
		{
			const uint32_t numVertices = _numIndices*4/6;
			if (0 < numVertices)
			{
				m_indexBuffers [_blitter.m_ib->handle.idx].update(m_commandList, 0, _numIndices*2, _blitter.m_ib->data);
				m_vertexBuffers[_blitter.m_vb->handle.idx].update(m_commandList, 0, numVertices*_blitter.m_decl.m_stride, _blitter.m_vb->data, true);

				m_commandList->DrawIndexedInstanced(_numIndices
					, 1
					, 0
					, 0
					, 0
					);
			}
		}

		void preReset()
		{
			finishAll();

			for (uint32_t ii = 0, num = m_scd.BufferCount; ii < num; ++ii)
			{
				DX_RELEASE(m_backBufferColor[ii], num-1-ii);
			}
			DX_RELEASE(m_backBufferDepthStencil, 0);

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
			{
				m_frameBuffers[ii].preReset();
			}

			invalidateCache();

//			capturePreReset();
		}

		void postReset()
		{
			memset(m_backBufferColorFence, 0, sizeof(m_backBufferColorFence) );

			uint32_t rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			for (uint32_t ii = 0, num = m_scd.BufferCount; ii < num; ++ii)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
				handle.ptr += ii * rtvDescriptorSize;
				DX_CHECK(m_swapChain->GetBuffer(ii
						, IID_ID3D12Resource
						, (void**)&m_backBufferColor[ii]
						) );
				m_device->CreateRenderTargetView(m_backBufferColor[ii], NULL, handle);
			}

			D3D12_RESOURCE_DESC resourceDesc;
			resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			resourceDesc.Alignment = 0;
			resourceDesc.Width     = bx::uint32_max(m_resolution.m_width,  1);
			resourceDesc.Height    = bx::uint32_max(m_resolution.m_height, 1);
			resourceDesc.DepthOrArraySize   = 1;
			resourceDesc.MipLevels          = 0;
			resourceDesc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
			resourceDesc.SampleDesc.Count   = 1;
			resourceDesc.SampleDesc.Quality = 0;
			resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			resourceDesc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clearValue;
			clearValue.Format = resourceDesc.Format;
			clearValue.DepthStencil.Depth   = 1.0f;
			clearValue.DepthStencil.Stencil = 0;

			m_backBufferDepthStencil = createCommittedResource(m_device, HeapProperty::Default, &resourceDesc, &clearValue);

			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
			ZeroMemory(&dsvDesc, sizeof(dsvDesc) );
			dsvDesc.Format        = resourceDesc.Format;
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			dsvDesc.Flags         = D3D12_DSV_FLAGS(0)
// 				| D3D12_DSV_FLAG_READ_ONLY_DEPTH
// 				| D3D12_DSV_FLAG_READ_ONLY_DEPTH
				;

			m_device->CreateDepthStencilView(m_backBufferDepthStencil
				, &dsvDesc
				, m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart()
				);

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
			{
				m_frameBuffers[ii].postReset();
			}

			m_commandList = m_cmd.alloc();
//			capturePostReset();
		}

		void invalidateCache()
		{
			m_pipelineStateCache.invalidate();

			m_samplerStateCache.invalidate();
			m_samplerAllocator.reset();
		}

		void updateMsaa()
		{
			for (uint32_t ii = 1, last = 0; ii < BX_COUNTOF(s_msaa); ++ii)
			{
				uint32_t msaa = s_checkMsaa[ii];

				D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data;
				memset(&data, 0, sizeof(msaa) );
				data.Format = m_scd.BufferDesc.Format;
				data.SampleCount = msaa;
				data.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
				HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &data, sizeof(data) );

data.NumQualityLevels = 0;

				if (SUCCEEDED(hr)
				&&  0 < data.NumQualityLevels)
				{
					s_msaa[ii].Count   = data.SampleCount;
					s_msaa[ii].Quality = data.NumQualityLevels - 1;
					last = ii;
				}
				else
				{
					s_msaa[ii] = s_msaa[last];
				}
			}
		}

		void updateResolution(const Resolution& _resolution)
		{
			if ( (uint32_t)m_scd.BufferDesc.Width != _resolution.m_width
			||   (uint32_t)m_scd.BufferDesc.Height != _resolution.m_height
			||   m_flags != _resolution.m_flags)
			{
				bool resize = (m_flags&BGFX_RESET_MSAA_MASK) == (_resolution.m_flags&BGFX_RESET_MSAA_MASK);
				m_flags = _resolution.m_flags;

				m_textVideoMem.resize(false, _resolution.m_width, _resolution.m_height);
				m_textVideoMem.clear();

				m_resolution = _resolution;
				m_scd.BufferDesc.Width  = _resolution.m_width;
				m_scd.BufferDesc.Height = _resolution.m_height;

				preReset();

				if (resize)
				{
					uint32_t nodeMask[] = { 1, 1, 1, 1 };
					BX_STATIC_ASSERT(BX_COUNTOF(m_backBufferColor) == BX_COUNTOF(nodeMask) );
					IUnknown* presentQueue[] ={ m_cmd.m_commandQueue, m_cmd.m_commandQueue, m_cmd.m_commandQueue, m_cmd.m_commandQueue };
					BX_STATIC_ASSERT(BX_COUNTOF(m_backBufferColor) == BX_COUNTOF(presentQueue) );

					DX_CHECK(m_swapChain->ResizeBuffers1(m_scd.BufferCount
							, m_scd.BufferDesc.Width
							, m_scd.BufferDesc.Height
							, m_scd.BufferDesc.Format
							, m_scd.Flags
							, nodeMask
							, presentQueue
							) );
				}
				else
				{
					updateMsaa();
					m_scd.SampleDesc = s_msaa[(m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT];

					DX_RELEASE(m_swapChain, 0);

					HRESULT hr;
					hr = m_factory->CreateSwapChain(m_cmd.m_commandQueue
							, &m_scd
							, reinterpret_cast<IDXGISwapChain**>(&m_swapChain)
							);
					BGFX_FATAL(SUCCEEDED(hr), bgfx::Fatal::UnableToInitialize, "Failed to create swap chain.");
				}

				postReset();
			}
		}

		void setShaderUniform(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			if (_flags&BGFX_UNIFORM_FRAGMENTBIT)
			{
				memcpy(&m_fsScratch[_regIndex], _val, _numRegs*16);
				m_fsChanges += _numRegs;
			}
			else
			{
				memcpy(&m_vsScratch[_regIndex], _val, _numRegs*16);
				m_vsChanges += _numRegs;
			}
		}

		void setShaderUniform4f(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			setShaderUniform(_flags, _regIndex, _val, _numRegs);
		}

		void setShaderUniform4x4f(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			setShaderUniform(_flags, _regIndex, _val, _numRegs);
		}

		void commitShaderConstants(uint16_t _programIdx, D3D12_GPU_VIRTUAL_ADDRESS& _gpuAddress)
		{
			ProgramD3D12& program = m_program[_programIdx];
			uint32_t total = bx::strideAlign(0
				+ program.m_vsh->m_size
				+ (NULL != program.m_fsh ? program.m_fsh->m_size : 0)
				, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT
				);
			uint8_t* data = (uint8_t*)m_scratchBuffer[m_backBufferColorIdx].allocCbv(_gpuAddress, total);

			{
				uint32_t size = program.m_vsh->m_size;
				memcpy(data, m_vsScratch, size);
				data += size;

				m_vsChanges = 0;
			}

			if (NULL != program.m_fsh)
			{
				memcpy(data, m_fsScratch, program.m_fsh->m_size);

				m_fsChanges = 0;
			}
		}

		void setFrameBuffer(FrameBufferHandle _fbh, bool _msaa = true)
		{
			if (isValid(m_fbh)
			&&  m_fbh.idx != _fbh.idx)
			{
				const FrameBufferD3D12& frameBuffer = m_frameBuffers[m_fbh.idx];

				for (uint8_t ii = 0, num = frameBuffer.m_num; ii < num; ++ii)
				{
					TextureD3D12& texture = m_textures[frameBuffer.m_texture[ii].idx];
					texture.setState(m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}

				if (isValid(frameBuffer.m_depth) )
				{
					TextureD3D12& texture = m_textures[frameBuffer.m_depth.idx];
					const bool bufferOnly = 0 != (texture.m_flags&BGFX_TEXTURE_RT_BUFFER_ONLY);
					if (!bufferOnly)
					{
						texture.setState(m_commandList, D3D12_RESOURCE_STATE_DEPTH_READ);
					}
				}
			}

			if (!isValid(_fbh) )
			{
				m_rtvHandle = m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
				uint32_t rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				m_rtvHandle.ptr += m_backBufferColorIdx * rtvDescriptorSize;
				m_dsvHandle = m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

				m_currentColor        = &m_rtvHandle;
				m_currentDepthStencil = &m_dsvHandle;
				m_commandList->OMSetRenderTargets(1, m_currentColor, false, m_currentDepthStencil);
			}
			else
			{
				const FrameBufferD3D12& frameBuffer = m_frameBuffers[_fbh.idx];

				if (0 < frameBuffer.m_num)
				{
					D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor = m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
					uint32_t rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
					m_rtvHandle.ptr = rtvDescriptor.ptr + (BX_COUNTOF(m_backBufferColor) + _fbh.idx * BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) * rtvDescriptorSize;
					m_currentColor  = &m_rtvHandle;
				}
				else
				{
					m_currentColor = NULL;
				}

				if (isValid(frameBuffer.m_depth) )
				{
					D3D12_CPU_DESCRIPTOR_HANDLE dsvDescriptor = m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
					uint32_t dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
					m_dsvHandle.ptr = dsvDescriptor.ptr + (1 + _fbh.idx) * dsvDescriptorSize;
					m_currentDepthStencil = &m_dsvHandle;
				}
				else
				{
					m_currentDepthStencil = NULL;
				}

				for (uint8_t ii = 0, num = frameBuffer.m_num; ii < num; ++ii)
				{
					TextureD3D12& texture = m_textures[frameBuffer.m_texture[ii].idx];
					texture.setState(m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
				}

				if (isValid(frameBuffer.m_depth) )
				{
					TextureD3D12& texture = m_textures[frameBuffer.m_depth.idx];
					texture.setState(m_commandList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				}

				m_commandList->OMSetRenderTargets(frameBuffer.m_num
												, m_currentColor
												, true //NULL == m_currentDepthStencil
												, m_currentDepthStencil
												);
			}

			m_fbh = _fbh;
			m_rtMsaa = _msaa;
		}

		void setBlendState(D3D12_BLEND_DESC& desc, uint64_t _state, uint32_t _rgba = 0)
		{
			memset(&desc, 0, sizeof(desc) );
			desc.IndependentBlendEnable = !!(BGFX_STATE_BLEND_INDEPENDENT & _state);

			D3D12_RENDER_TARGET_BLEND_DESC* drt = &desc.RenderTarget[0];
			drt->BlendEnable = !!(BGFX_STATE_BLEND_MASK & _state);

			{
				const uint32_t blend    = uint32_t( (_state & BGFX_STATE_BLEND_MASK         ) >> BGFX_STATE_BLEND_SHIFT);
				const uint32_t equation = uint32_t( (_state & BGFX_STATE_BLEND_EQUATION_MASK) >> BGFX_STATE_BLEND_EQUATION_SHIFT);

				const uint32_t srcRGB = (blend      ) & 0xf;
				const uint32_t dstRGB = (blend >>  4) & 0xf;
				const uint32_t srcA   = (blend >>  8) & 0xf;
				const uint32_t dstA   = (blend >> 12) & 0xf;

				const uint32_t equRGB = (equation     ) & 0x7;
				const uint32_t equA   = (equation >> 3) & 0x7;

				drt->SrcBlend       = s_blendFactor[srcRGB][0];
				drt->DestBlend      = s_blendFactor[dstRGB][0];
				drt->BlendOp        = s_blendEquation[equRGB];

				drt->SrcBlendAlpha  = s_blendFactor[srcA][1];
				drt->DestBlendAlpha = s_blendFactor[dstA][1];
				drt->BlendOpAlpha   = s_blendEquation[equA];
			}

			uint8_t writeMask = (_state & BGFX_STATE_ALPHA_WRITE)
					? D3D12_COLOR_WRITE_ENABLE_ALPHA
					: 0
					;
			writeMask |= (_state & BGFX_STATE_RGB_WRITE)
					? D3D12_COLOR_WRITE_ENABLE_RED
					| D3D12_COLOR_WRITE_ENABLE_GREEN
					| D3D12_COLOR_WRITE_ENABLE_BLUE
					: 0
					;

			drt->RenderTargetWriteMask = writeMask;

			if (desc.IndependentBlendEnable)
			{
				for (uint32_t ii = 1, rgba = _rgba; ii < BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS; ++ii, rgba >>= 11)
				{
					drt = &desc.RenderTarget[ii];
					drt->BlendEnable = 0 != (rgba & 0x7ff);

					const uint32_t src      = (rgba     ) & 0xf;
					const uint32_t dst      = (rgba >> 4) & 0xf;
					const uint32_t equation = (rgba >> 8) & 0x7;

					drt->SrcBlend       = s_blendFactor[src][0];
					drt->DestBlend      = s_blendFactor[dst][0];
					drt->BlendOp        = s_blendEquation[equation];

					drt->SrcBlendAlpha  = s_blendFactor[src][1];
					drt->DestBlendAlpha = s_blendFactor[dst][1];
					drt->BlendOpAlpha   = s_blendEquation[equation];

					drt->RenderTargetWriteMask = writeMask;
				}
			}
			else
			{
				for (uint32_t ii = 1; ii < BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS; ++ii)
				{
					memcpy(&desc.RenderTarget[ii], drt, sizeof(D3D12_RENDER_TARGET_BLEND_DESC) );
				}
			}
		}

		void setRasterizerState(D3D12_RASTERIZER_DESC& desc, uint64_t _state, bool _wireframe = false)
		{
			const uint32_t cull = (_state&BGFX_STATE_CULL_MASK) >> BGFX_STATE_CULL_SHIFT;

			desc.FillMode = _wireframe
				? D3D12_FILL_MODE_WIREFRAME
				: D3D12_FILL_MODE_SOLID
				;
			desc.CullMode = s_cullMode[cull];
			desc.FrontCounterClockwise = false;
			desc.DepthBias = 0;
			desc.DepthBiasClamp = 0.0f;
			desc.SlopeScaledDepthBias = 0.0f;
			desc.DepthClipEnable = false;
			desc.MultisampleEnable = !!(_state&BGFX_STATE_MSAA);
			desc.AntialiasedLineEnable = false;
			desc.ForcedSampleCount = 0;
			desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
		}

		void setDepthStencilState(D3D12_DEPTH_STENCIL_DESC& desc, uint64_t _state, uint64_t _stencil = 0)
		{
			const uint32_t fstencil = unpackStencil(0, _stencil);

			memset(&desc, 0, sizeof(desc) );
			uint32_t func = (_state&BGFX_STATE_DEPTH_TEST_MASK)>>BGFX_STATE_DEPTH_TEST_SHIFT;
			desc.DepthEnable = 0 != func;
			desc.DepthWriteMask = !!(BGFX_STATE_DEPTH_WRITE & _state)
				? D3D12_DEPTH_WRITE_MASK_ALL
				: D3D12_DEPTH_WRITE_MASK_ZERO
				;
			desc.DepthFunc = s_cmpFunc[func];

			uint32_t bstencil = unpackStencil(1, _stencil);
			uint32_t frontAndBack = bstencil != BGFX_STENCIL_NONE && bstencil != fstencil;
			bstencil = frontAndBack ? bstencil : fstencil;

			desc.StencilEnable    = 0 != _stencil;
			desc.StencilReadMask  = (fstencil & BGFX_STENCIL_FUNC_RMASK_MASK) >> BGFX_STENCIL_FUNC_RMASK_SHIFT;
			desc.StencilWriteMask = 0xff;

			desc.FrontFace.StencilFailOp      = s_stencilOp[(fstencil & BGFX_STENCIL_OP_FAIL_S_MASK) >> BGFX_STENCIL_OP_FAIL_S_SHIFT];
			desc.FrontFace.StencilDepthFailOp = s_stencilOp[(fstencil & BGFX_STENCIL_OP_FAIL_Z_MASK) >> BGFX_STENCIL_OP_FAIL_Z_SHIFT];
			desc.FrontFace.StencilPassOp      = s_stencilOp[(fstencil & BGFX_STENCIL_OP_PASS_Z_MASK) >> BGFX_STENCIL_OP_PASS_Z_SHIFT];
			desc.FrontFace.StencilFunc        = s_cmpFunc[(fstencil & BGFX_STENCIL_TEST_MASK) >> BGFX_STENCIL_TEST_SHIFT];

			desc.BackFace.StencilFailOp       = s_stencilOp[(bstencil & BGFX_STENCIL_OP_FAIL_S_MASK) >> BGFX_STENCIL_OP_FAIL_S_SHIFT];
			desc.BackFace.StencilDepthFailOp  = s_stencilOp[(bstencil & BGFX_STENCIL_OP_FAIL_Z_MASK) >> BGFX_STENCIL_OP_FAIL_Z_SHIFT];
			desc.BackFace.StencilPassOp       = s_stencilOp[(bstencil & BGFX_STENCIL_OP_PASS_Z_MASK) >> BGFX_STENCIL_OP_PASS_Z_SHIFT];
			desc.BackFace.StencilFunc         = s_cmpFunc[(bstencil&BGFX_STENCIL_TEST_MASK) >> BGFX_STENCIL_TEST_SHIFT];
		}

		uint32_t setInputLayout(D3D12_INPUT_ELEMENT_DESC* _vertexElements, const VertexDecl& _vertexDecl, const ProgramD3D12& _program, uint8_t _numInstanceData)
		{
			VertexDecl decl;
			memcpy(&decl, &_vertexDecl, sizeof(VertexDecl) );
			const uint16_t* attrMask = _program.m_vsh->m_attrMask;

			for (uint32_t ii = 0; ii < Attrib::Count; ++ii)
			{
				uint16_t mask = attrMask[ii];
				uint16_t attr = (decl.m_attributes[ii] & mask);
				decl.m_attributes[ii] = attr == 0 ? UINT16_MAX : attr == UINT16_MAX ? 0 : attr;
			}

			D3D12_INPUT_ELEMENT_DESC* elem = fillVertexDecl(_vertexElements, decl);
			uint32_t num = uint32_t(elem-_vertexElements);

			const D3D12_INPUT_ELEMENT_DESC inst = { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 };

			for (uint32_t ii = 0; ii < _numInstanceData; ++ii)
			{
				uint32_t index = 7 - ii; // TEXCOORD7 = i_data0, TEXCOORD6 = i_data1, etc.

				uint32_t jj;
				D3D12_INPUT_ELEMENT_DESC* curr = _vertexElements;
				for (jj = 0; jj < num; ++jj)
				{
					curr = &_vertexElements[jj];
					if (0 == strcmp(curr->SemanticName, "TEXCOORD")
					&&  curr->SemanticIndex == index)
					{
						break;
					}
				}

				if (jj == num)
				{
					curr = elem;
					++elem;
				}

				memcpy(curr, &inst, sizeof(D3D12_INPUT_ELEMENT_DESC) );
				curr->InputSlot = 1;
				curr->SemanticIndex = index;
				curr->AlignedByteOffset = ii*16;
			}

			return uint32_t(elem-_vertexElements);
		}

		static void patchCb0(DxbcInstruction& _instruction, void* _userData)
		{
			union { void* ptr; uint32_t offset; } cast = { _userData };

			for (uint32_t ii = 0; ii < _instruction.numOperands; ++ii)
			{
				DxbcOperand& operand = _instruction.operand[ii];
				if (DxbcOperandType::ConstantBuffer == operand.type)
				{
					if (DxbcOperandAddrMode::Imm32 == operand.addrMode[0]
					&&  0 == operand.regIndex[0]
					&&  DxbcOperandAddrMode::Imm32 == operand.addrMode[1])
					{
						operand.regIndex[1] += cast.offset;
					}
				}
			}
		}

		ID3D12PipelineState* getPipelineState(uint16_t _programIdx)
		{
			ProgramD3D12& program = m_program[_programIdx];

			const uint32_t hash = program.m_vsh->m_hash;

			ID3D12PipelineState* pso = m_pipelineStateCache.find(hash);

			if (BX_LIKELY(NULL != pso) )
			{
				return pso;
			}

			D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
			memset(&desc, 0, sizeof(desc) );

			desc.pRootSignature = m_rootSignature;

			desc.CS.pShaderBytecode = program.m_vsh->m_code->data;
			desc.CS.BytecodeLength  = program.m_vsh->m_code->size;

			DX_CHECK(m_device->CreateComputePipelineState(&desc
				, IID_ID3D12PipelineState
				, (void**)&pso
				) );
			m_pipelineStateCache.add(hash, pso);

			return pso;
		}

		ID3D12PipelineState* getPipelineState(uint64_t _state, uint64_t _stencil, uint16_t _declIdx, uint16_t _programIdx, uint8_t _numInstanceData)
		{
			ProgramD3D12& program = m_program[_programIdx];

			_state &= 0
				| BGFX_STATE_RGB_WRITE
				| BGFX_STATE_ALPHA_WRITE
				| BGFX_STATE_DEPTH_WRITE
				| BGFX_STATE_DEPTH_TEST_MASK
				| BGFX_STATE_BLEND_MASK
				| BGFX_STATE_BLEND_EQUATION_MASK
				| BGFX_STATE_BLEND_INDEPENDENT
				| BGFX_STATE_CULL_MASK
				| BGFX_STATE_MSAA
				| BGFX_STATE_PT_MASK
				;

			_stencil &= packStencil(~BGFX_STENCIL_FUNC_REF_MASK, BGFX_STENCIL_MASK);

			VertexDecl decl;
			memcpy(&decl, &m_vertexDecls[_declIdx], sizeof(VertexDecl) );
			const uint16_t* attrMask = program.m_vsh->m_attrMask;

			for (uint32_t ii = 0; ii < Attrib::Count; ++ii)
			{
				uint16_t mask = attrMask[ii];
				uint16_t attr = (decl.m_attributes[ii] & mask);
				decl.m_attributes[ii] = attr == 0 ? UINT16_MAX : attr == UINT16_MAX ? 0 : attr;
			}

			bx::HashMurmur2A murmur;
			murmur.begin();
			murmur.add(_state);
			murmur.add(_stencil);
			murmur.add(program.m_vsh->m_hash);
			murmur.add(program.m_vsh->m_attrMask, sizeof(program.m_vsh->m_attrMask) );
			murmur.add(program.m_fsh->m_hash);
			murmur.add(m_vertexDecls[_declIdx].m_hash);
			murmur.add(decl.m_attributes, sizeof(decl.m_attributes) );
			murmur.add(m_fbh.idx);
			murmur.add(_numInstanceData);
			const uint32_t hash = murmur.end();

			ID3D12PipelineState* pso = m_pipelineStateCache.find(hash);

			if (NULL != pso)
			{
				return pso;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
			memset(&desc, 0, sizeof(desc) );

			desc.pRootSignature = m_rootSignature;

			desc.VS.pShaderBytecode = program.m_vsh->m_code->data;
			desc.VS.BytecodeLength  = program.m_vsh->m_code->size;

 			const Memory* temp = alloc(program.m_fsh->m_code->size);
 			memset(temp->data, 0, temp->size);
 			bx::MemoryReader rd(program.m_fsh->m_code->data, program.m_fsh->m_code->size);
 			bx::StaticMemoryBlockWriter wr(temp->data, temp->size);

			DxbcContext dxbc;
			read(&rd, dxbc);

			bool patchShader = true;
			if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
			{
				union { uint32_t offset; void* ptr; } cast = { 0 };
				filter(dxbc.shader, dxbc.shader, patchCb0, cast.ptr);

				write(&wr, dxbc);

				dxbcHash(temp->data + 20, temp->size - 20, temp->data + 4);

				patchShader = 0 == memcmp(program.m_fsh->m_code->data, temp->data, 16);
				BX_CHECK(patchShader, "DXBC fragment shader patching error (ShaderHandle: %d).", program.m_fsh - m_shaders);

				if (!patchShader)
				{
					for (uint32_t ii = 20; ii < temp->size; ii += 16)
					{
						if (0 != memcmp(&program.m_fsh->m_code->data[ii], &temp->data[ii], 16) )
						{
// 							dbgPrintfData(&program.m_fsh->m_code->data[ii], temp->size-ii, "");
// 							dbgPrintfData(&temp->data[ii], temp->size-ii, "");
							break;
						}
					}

					desc.PS.pShaderBytecode = program.m_fsh->m_code->data;
					desc.PS.BytecodeLength  = program.m_fsh->m_code->size;
				}
			}

			if (patchShader)
			{
				memcpy(temp->data, program.m_fsh->m_code->data, program.m_fsh->m_code->size);

				bx::seek(&wr, 0, bx::Whence::Begin);
				union { uint32_t offset; void* ptr; } cast =
				{
					uint32_t(program.m_vsh->m_size)/16
				};
				filter(dxbc.shader, dxbc.shader, patchCb0, cast.ptr);
				write(&wr, dxbc);
				dxbcHash(temp->data + 20, temp->size - 20, temp->data + 4);

				desc.PS.pShaderBytecode = temp->data;
				desc.PS.BytecodeLength  = temp->size;
			}

			desc.DS.pShaderBytecode = NULL;
			desc.DS.BytecodeLength  = 0;

			desc.HS.pShaderBytecode = NULL;
			desc.HS.BytecodeLength  = 0;

			desc.GS.pShaderBytecode = NULL;
			desc.GS.BytecodeLength  = 0;

			desc.StreamOutput.pSODeclaration   = NULL;
			desc.StreamOutput.NumEntries       = 0;
			desc.StreamOutput.pBufferStrides   = NULL;
			desc.StreamOutput.NumStrides       = 0;
			desc.StreamOutput.RasterizedStream = 0;

			setBlendState(desc.BlendState, _state);
			desc.SampleMask = 1;
			setRasterizerState(desc.RasterizerState, _state);
			setDepthStencilState(desc.DepthStencilState, _state, _stencil);

			D3D12_INPUT_ELEMENT_DESC vertexElements[Attrib::Count + 1 + BGFX_CONFIG_MAX_INSTANCE_DATA_COUNT];
			desc.InputLayout.NumElements = setInputLayout(vertexElements, m_vertexDecls[_declIdx], program, _numInstanceData);
			desc.InputLayout.pInputElementDescs = vertexElements;

			uint8_t primIndex = uint8_t( (_state&BGFX_STATE_PT_MASK) >> BGFX_STATE_PT_SHIFT);
			desc.PrimitiveTopologyType = s_primInfo[primIndex].m_topologyType;

			if (isValid(m_fbh) )
			{
				const FrameBufferD3D12& frameBuffer = m_frameBuffers[m_fbh.idx];
				desc.NumRenderTargets = frameBuffer.m_num;

				for (uint8_t ii = 0, num = frameBuffer.m_num; ii < num; ++ii)
				{
					desc.RTVFormats[ii] = m_textures[frameBuffer.m_texture[ii].idx].m_srvd.Format;
				}

				if (isValid(frameBuffer.m_depth) )
				{
					desc.DSVFormat = s_textureFormat[m_textures[frameBuffer.m_depth.idx].m_textureFormat].m_fmtDsv;
				}
				else
				{
					desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
				}
			}
			else
			{
				desc.NumRenderTargets = 1;
				desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.DSVFormat     = DXGI_FORMAT_D24_UNORM_S8_UINT;
			}

			desc.SampleDesc.Count   = 1;
			desc.SampleDesc.Quality = 0;

			uint32_t length = g_callback->cacheReadSize(hash);
			bool cached = length > 0;

			void* cachedData = NULL;

			if (cached)
			{
				cachedData = BX_ALLOC(g_allocator, length);
				if (g_callback->cacheRead(hash, cachedData, length) )
				{
					BX_TRACE("Loading cached PSO (size %d).", length);
					bx::MemoryReader reader(cachedData, length);

					desc.CachedPSO.pCachedBlob           = reader.getDataPtr();
					desc.CachedPSO.CachedBlobSizeInBytes = (size_t)reader.remaining();

					HRESULT hr = m_device->CreateGraphicsPipelineState(&desc
									, IID_ID3D12PipelineState
									, (void**)&pso
									);
					if (FAILED(hr) )
					{
						BX_TRACE("Failed to load cached PSO (HRESULT 0x%08x).", hr);
						memset(&desc.CachedPSO, 0, sizeof(desc.CachedPSO) );
					}
				}
			}

			if (NULL == pso)
			{
				DX_CHECK(m_device->CreateGraphicsPipelineState(&desc
						, IID_ID3D12PipelineState
						, (void**)&pso
						) );
			}
			m_pipelineStateCache.add(hash, pso);

			release(temp);

			ID3DBlob* blob;
			HRESULT hr = pso->GetCachedBlob(&blob);
			if (SUCCEEDED(hr) )
			{
				void* data = blob->GetBufferPointer();
				length = (uint32_t)blob->GetBufferSize();

				g_callback->cacheWrite(hash, data, length);

				DX_RELEASE(blob, 0);
			}

			if (NULL != cachedData)
			{
				BX_FREE(g_allocator, cachedData);
			}

			return pso;
		}

		uint16_t getSamplerState(const uint32_t* _flags, uint32_t _num = BGFX_CONFIG_MAX_TEXTURE_SAMPLERS)
		{
			bx::HashMurmur2A murmur;
			murmur.begin();
			murmur.add(_flags, _num * sizeof(uint32_t) );
			uint32_t hash = murmur.end();

			uint16_t sampler = m_samplerStateCache.find(hash);
			if (UINT16_MAX == sampler)
			{
				sampler = m_samplerAllocator.alloc(_flags, _num);
				m_samplerStateCache.add(hash, sampler);
			}

			return sampler;
		}

		void commit(ConstantBuffer& _constantBuffer)
		{
			_constantBuffer.reset();

			for (;;)
			{
				uint32_t opcode = _constantBuffer.read();

				if (UniformType::End == opcode)
				{
					break;
				}

				UniformType::Enum type;
				uint16_t loc;
				uint16_t num;
				uint16_t copy;
				ConstantBuffer::decodeOpcode(opcode, type, loc, num, copy);

				const char* data;
				if (copy)
				{
					data = _constantBuffer.read(g_uniformTypeSize[type]*num);
				}
				else
				{
					UniformHandle handle;
					memcpy(&handle, _constantBuffer.read(sizeof(UniformHandle) ), sizeof(UniformHandle) );
					data = (const char*)m_uniforms[handle.idx];
				}

#define CASE_IMPLEMENT_UNIFORM(_uniform, _dxsuffix, _type) \
				case UniformType::_uniform: \
				case UniformType::_uniform|BGFX_UNIFORM_FRAGMENTBIT: \
						{ \
							setShaderUniform(uint8_t(type), loc, data, num); \
						} \
						break;

				switch ( (uint32_t)type)
				{
				case UniformType::Mat3:
				case UniformType::Mat3|BGFX_UNIFORM_FRAGMENTBIT:
					 {
						 float* value = (float*)data;
						 for (uint32_t ii = 0, count = num/3; ii < count; ++ii,  loc += 3*16, value += 9)
						 {
							 Matrix4 mtx;
							 mtx.un.val[ 0] = value[0];
							 mtx.un.val[ 1] = value[1];
							 mtx.un.val[ 2] = value[2];
							 mtx.un.val[ 3] = 0.0f;
							 mtx.un.val[ 4] = value[3];
							 mtx.un.val[ 5] = value[4];
							 mtx.un.val[ 6] = value[5];
							 mtx.un.val[ 7] = 0.0f;
							 mtx.un.val[ 8] = value[6];
							 mtx.un.val[ 9] = value[7];
							 mtx.un.val[10] = value[8];
							 mtx.un.val[11] = 0.0f;
							 setShaderUniform(uint8_t(type), loc, &mtx.un.val[0], 3);
						 }
					}
					break;

				CASE_IMPLEMENT_UNIFORM(Int1, I, int);
				CASE_IMPLEMENT_UNIFORM(Vec4, F, float);
				CASE_IMPLEMENT_UNIFORM(Mat4, F, float);

				case UniformType::End:
					break;

				default:
					BX_TRACE("%4d: INVALID 0x%08x, t %d, l %d, n %d, c %d", _constantBuffer.getPos(), opcode, type, loc, num, copy);
					break;
				}
#undef CASE_IMPLEMENT_UNIFORM
			}
		}

		void clear(const Clear& _clear, const float _palette[][4], const D3D12_RECT* _rect = NULL, uint32_t _num = 0)
		{
			if (isValid(m_fbh) )
			{
				FrameBufferD3D12& frameBuffer = m_frameBuffers[m_fbh.idx];
				frameBuffer.clear(m_commandList, _clear, _palette);
			}
			else
			{
				if (NULL != m_currentColor
				&&  BGFX_CLEAR_COLOR & _clear.m_flags)
				{
					if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
					{
						uint8_t index = _clear.m_index[0];
						if (UINT8_MAX != index)
						{
							m_commandList->ClearRenderTargetView(*m_currentColor
								, _palette[index]
								, _num
								, _rect
								);
						}
					}
					else
					{
						float frgba[4] =
						{
							_clear.m_index[0] * 1.0f / 255.0f,
							_clear.m_index[1] * 1.0f / 255.0f,
							_clear.m_index[2] * 1.0f / 255.0f,
							_clear.m_index[3] * 1.0f / 255.0f,
						};
						m_commandList->ClearRenderTargetView(*m_currentColor
							, frgba
							, _num
							, _rect
							);
					}
				}

				if (NULL != m_currentDepthStencil
				&& (BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL) & _clear.m_flags)
				{
					uint32_t flags = 0;
					flags |= (_clear.m_flags & BGFX_CLEAR_DEPTH  ) ? D3D12_CLEAR_FLAG_DEPTH   : 0;
					flags |= (_clear.m_flags & BGFX_CLEAR_STENCIL) ? D3D12_CLEAR_FLAG_STENCIL : 0;

					m_commandList->ClearDepthStencilView(*m_currentDepthStencil
						, D3D12_CLEAR_FLAGS(flags)
						, _clear.m_depth
						, _clear.m_stencil
						, _num
						, _rect
						);
				}
			}
		}

		void clearQuad(const Rect& _rect, const Clear& _clear, const float _palette[][4])
		{
			uint32_t width  = m_scd.BufferDesc.Width;
			uint32_t height = m_scd.BufferDesc.Height;

			if (0      == _rect.m_x
			&&  0      == _rect.m_y
			&&  width  == _rect.m_width
			&&  height == _rect.m_height)
			{
				clear(_clear, _palette);
			}
			else
			{
				D3D12_RECT rect;
				rect.left   = _rect.m_x;
				rect.top    = _rect.m_y;
				rect.right  = _rect.m_x + _rect.m_width;
				rect.bottom = _rect.m_y + _rect.m_height;
				clear(_clear, _palette, &rect);
			}
		}

		uint64_t kick()
		{
			uint64_t fence = m_cmd.kick();
			m_commandList = m_cmd.alloc();
			return fence;
		}

		void finish()
		{
			m_cmd.kick();
			m_cmd.finish();
			m_commandList = NULL;
		}

		void finishAll()
		{
			uint64_t fence = m_cmd.kick();
			m_cmd.finish(fence, true);
			m_commandList = NULL;
		}

		void* m_kernel32dll;
		void* m_d3d12dll;
		void* m_dxgidll;

		D3D_DRIVER_TYPE m_driverType;
		IDXGIAdapter3* m_adapter;
		DXGI_ADAPTER_DESC m_adapterDesc;
		D3D12_FEATURE_DATA_ARCHITECTURE m_architecture;
		D3D12_FEATURE_DATA_D3D12_OPTIONS m_options;

		IDXGIFactory4* m_factory;

		IDXGISwapChain3* m_swapChain;
		int64_t m_presentElapsed;
		uint16_t m_lost;
		uint16_t m_numWindows;
		FrameBufferHandle m_windows[BGFX_CONFIG_MAX_FRAME_BUFFERS];

		ID3D12Device* m_device;
		ID3D12InfoQueue* m_infoQueue;

		ID3D12DescriptorHeap* m_rtvDescriptorHeap;
		ID3D12DescriptorHeap* m_dsvDescriptorHeap;
		D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle;
		D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle;
		D3D12_CPU_DESCRIPTOR_HANDLE* m_currentColor;
		D3D12_CPU_DESCRIPTOR_HANDLE* m_currentDepthStencil;
		ID3D12Resource* m_backBufferColor[4];
		uint64_t m_backBufferColorFence[4];
		ID3D12Resource* m_backBufferDepthStencil;

		ScratchBufferD3D12 m_scratchBuffer[4];
		DescriptorAllocatorD3D12 m_samplerAllocator;

		ID3D12RootSignature* m_rootSignature;

		CommandQueueD3D12 m_cmd;
		BatchD3D12 m_batch;
		ID3D12GraphicsCommandList* m_commandList;

		Resolution m_resolution;
		bool m_wireframe;

		DXGI_SWAP_CHAIN_DESC m_scd;
		uint32_t m_flags;

		BufferD3D12 m_indexBuffers[BGFX_CONFIG_MAX_INDEX_BUFFERS];
		VertexBufferD3D12 m_vertexBuffers[BGFX_CONFIG_MAX_VERTEX_BUFFERS];
		ShaderD3D12 m_shaders[BGFX_CONFIG_MAX_SHADERS];
		ProgramD3D12 m_program[BGFX_CONFIG_MAX_PROGRAMS];
		TextureD3D12 m_textures[BGFX_CONFIG_MAX_TEXTURES];
		VertexDecl m_vertexDecls[BGFX_CONFIG_MAX_VERTEX_DECLS];
		FrameBufferD3D12 m_frameBuffers[BGFX_CONFIG_MAX_FRAME_BUFFERS];
		void* m_uniforms[BGFX_CONFIG_MAX_UNIFORMS];
		Matrix4 m_predefinedUniforms[PredefinedUniform::Count];
		UniformRegistry m_uniformReg;

		StateCacheT<ID3D12PipelineState> m_pipelineStateCache;
		StateCache m_samplerStateCache;

		TextVideoMem m_textVideoMem;

		uint8_t m_fsScratch[64<<10];
		uint8_t m_vsScratch[64<<10];
		uint32_t m_fsChanges;
		uint32_t m_vsChanges;

		FrameBufferHandle m_fbh;
		uint32_t m_backBufferColorIdx;
		bool m_rtMsaa;
	};

	static RendererContextD3D12* s_renderD3D12;

	RendererContextI* rendererCreate()
	{
		s_renderD3D12 = BX_NEW(g_allocator, RendererContextD3D12);
		if (!s_renderD3D12->init() )
		{
			BX_DELETE(g_allocator, s_renderD3D12);
			s_renderD3D12 = NULL;
		}
		return s_renderD3D12;
	}

	void rendererDestroy()
	{
		s_renderD3D12->shutdown();
		BX_DELETE(g_allocator, s_renderD3D12);
		s_renderD3D12 = NULL;
	}

	void ScratchBufferD3D12::create(uint32_t _size, uint32_t _maxDescriptors)
	{
		m_size = _size;

		ID3D12Device* device = s_renderD3D12->m_device;

		m_incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_DESCRIPTOR_HEAP_DESC desc;
		desc.NumDescriptors = _maxDescriptors;
		desc.Type     = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.Flags    = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NodeMask = 1;
		DX_CHECK(device->CreateDescriptorHeap(&desc
				, IID_ID3D12DescriptorHeap
				, (void**)&m_heap
				) );

		m_upload = createCommittedResource(device, HeapProperty::Upload, desc.NumDescriptors * 1024);
		m_gpuVA  = m_upload->GetGPUVirtualAddress();
		m_upload->Map(0, NULL, (void**)&m_data);

		reset(m_gpuHandle);
	}

	void ScratchBufferD3D12::destroy()
	{
		m_upload->Unmap(0, NULL);

		DX_RELEASE(m_upload, 0);
		DX_RELEASE(m_heap, 0);
	}

	void ScratchBufferD3D12::reset(D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle)
	{
		m_pos = 0;
		m_cpuHandle = m_heap->GetCPUDescriptorHandleForHeapStart();
		m_gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
		gpuHandle = m_gpuHandle;
	}

	void* ScratchBufferD3D12::allocCbv(D3D12_GPU_VIRTUAL_ADDRESS& _gpuAddress, uint32_t _size)
	{
		_gpuAddress = m_gpuVA + m_pos;
		D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
		desc.BufferLocation = _gpuAddress;
		desc.SizeInBytes    = _size;

		void* data = &m_data[m_pos];

		m_pos += BX_ALIGN_256(_size);

		ID3D12Device* device = s_renderD3D12->m_device;
		device->CreateConstantBufferView(&desc
			, m_cpuHandle
			);
		m_cpuHandle.ptr += m_incrementSize;
		m_gpuHandle.ptr += m_incrementSize;

		return data;
	}

	void ScratchBufferD3D12::allocSrv(D3D12_GPU_DESCRIPTOR_HANDLE& _gpuHandle, TextureD3D12& _texture, uint8_t _mip)
	{
		ID3D12Device* device = s_renderD3D12->m_device;

		D3D12_SHADER_RESOURCE_VIEW_DESC tmpSrvd;
		D3D12_SHADER_RESOURCE_VIEW_DESC* srvd = &_texture.m_srvd;
		if (0 != _mip)
		{
			memcpy(&tmpSrvd, srvd, sizeof(tmpSrvd) );
			srvd = &tmpSrvd;

			switch (_texture.m_srvd.ViewDimension)
			{
			default:
			case D3D12_SRV_DIMENSION_TEXTURE2D:
				srvd->Texture2D.MostDetailedMip = _mip;
				srvd->Texture2D.MipLevels       = 1;
				srvd->Texture2D.PlaneSlice      = 0;
				srvd->Texture2D.ResourceMinLODClamp = 0;
				break;

			case D3D12_SRV_DIMENSION_TEXTURECUBE:
				srvd->TextureCube.MostDetailedMip = _mip;
				srvd->TextureCube.MipLevels       = 1;
				srvd->TextureCube.ResourceMinLODClamp = 0;
				break;

			case D3D12_SRV_DIMENSION_TEXTURE3D:
				srvd->Texture3D.MostDetailedMip = _mip;
				srvd->Texture3D.MipLevels       = 1;
				srvd->Texture3D.ResourceMinLODClamp = 0;
				break;
			}
		}

		device->CreateShaderResourceView(_texture.m_ptr
			, srvd
			, m_cpuHandle
			);
		m_cpuHandle.ptr += m_incrementSize;

		_gpuHandle = m_gpuHandle;
		m_gpuHandle.ptr += m_incrementSize;
	}

	void ScratchBufferD3D12::allocUav(D3D12_GPU_DESCRIPTOR_HANDLE& _gpuHandle, TextureD3D12& _texture, uint8_t _mip)
	{
		ID3D12Device* device = s_renderD3D12->m_device;

		D3D12_UNORDERED_ACCESS_VIEW_DESC tmpUavd;
		D3D12_UNORDERED_ACCESS_VIEW_DESC* uavd = &_texture.m_uavd;
		if (0 != _mip)
		{
			memcpy(&tmpUavd, uavd, sizeof(tmpUavd) );
			uavd = &tmpUavd;

			switch (_texture.m_uavd.ViewDimension)
			{
			default:
			case D3D12_UAV_DIMENSION_TEXTURE2D:
				uavd->Texture2D.MipSlice   = _mip;
				uavd->Texture2D.PlaneSlice = 0;
				break;

			case D3D12_UAV_DIMENSION_TEXTURE3D:
				uavd->Texture3D.MipSlice = _mip;
				break;
			}
		}

		device->CreateUnorderedAccessView(_texture.m_ptr
			, NULL
			, uavd
			, m_cpuHandle
			);
		m_cpuHandle.ptr += m_incrementSize;

		_gpuHandle = m_gpuHandle;
		m_gpuHandle.ptr += m_incrementSize;
	}

	void ScratchBufferD3D12::allocSrv(D3D12_GPU_DESCRIPTOR_HANDLE& _gpuHandle, BufferD3D12& _buffer)
	{
		ID3D12Device* device = s_renderD3D12->m_device;
		device->CreateShaderResourceView(_buffer.m_ptr
			, &_buffer.m_srvd
			, m_cpuHandle
			);
		m_cpuHandle.ptr += m_incrementSize;

		_gpuHandle = m_gpuHandle;
		m_gpuHandle.ptr += m_incrementSize;
	}

	void ScratchBufferD3D12::allocUav(D3D12_GPU_DESCRIPTOR_HANDLE& _gpuHandle, BufferD3D12& _buffer)
	{
		ID3D12Device* device = s_renderD3D12->m_device;
		device->CreateUnorderedAccessView(_buffer.m_ptr
			, NULL
			, &_buffer.m_uavd
			, m_cpuHandle
			);
		m_cpuHandle.ptr += m_incrementSize;

		_gpuHandle = m_gpuHandle;
		m_gpuHandle.ptr += m_incrementSize;
	}

	void DescriptorAllocatorD3D12::create(D3D12_DESCRIPTOR_HEAP_TYPE _type, uint16_t _maxDescriptors, uint16_t _numDescriptorsPerBlock)
	{
		m_handleAlloc = bx::createHandleAlloc(g_allocator, _maxDescriptors);
		m_numDescriptorsPerBlock = _numDescriptorsPerBlock;

		ID3D12Device* device = s_renderD3D12->m_device;

		m_incrementSize = device->GetDescriptorHandleIncrementSize(_type);

		D3D12_DESCRIPTOR_HEAP_DESC desc;
		desc.NumDescriptors = _maxDescriptors;
		desc.Type     = _type;
		desc.Flags    = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NodeMask = 1;
		DX_CHECK(device->CreateDescriptorHeap(&desc
				, IID_ID3D12DescriptorHeap
				, (void**)&m_heap
				) );

		m_cpuHandle = m_heap->GetCPUDescriptorHandleForHeapStart();
		m_gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
	}

	void DescriptorAllocatorD3D12::destroy()
	{
		bx::destroyHandleAlloc(g_allocator, m_handleAlloc);

		DX_RELEASE(m_heap, 0);
	}

	uint16_t DescriptorAllocatorD3D12::alloc(ID3D12Resource* _ptr, const D3D12_SHADER_RESOURCE_VIEW_DESC* _desc)
	{
		uint16_t idx = m_handleAlloc->alloc();

		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = { m_cpuHandle.ptr + idx * m_incrementSize };

		ID3D12Device* device = s_renderD3D12->m_device;
		device->CreateShaderResourceView(_ptr
			, _desc
			, cpuHandle
			);

		return idx;
	}

	uint16_t DescriptorAllocatorD3D12::alloc(const uint32_t* _flags, uint32_t _num)
	{
		uint16_t idx = m_handleAlloc->alloc();

		ID3D12Device* device = s_renderD3D12->m_device;

		for (uint32_t ii = 0; ii < _num; ++ii)
		{
			uint32_t flags = _flags[ii];

			const uint32_t cmpFunc   = (flags&BGFX_TEXTURE_COMPARE_MASK)>>BGFX_TEXTURE_COMPARE_SHIFT;
			const uint8_t  minFilter = s_textureFilter[0][(flags&BGFX_TEXTURE_MIN_MASK)>>BGFX_TEXTURE_MIN_SHIFT];
			const uint8_t  magFilter = s_textureFilter[1][(flags&BGFX_TEXTURE_MAG_MASK)>>BGFX_TEXTURE_MAG_SHIFT];
			const uint8_t  mipFilter = s_textureFilter[2][(flags&BGFX_TEXTURE_MIP_MASK)>>BGFX_TEXTURE_MIP_SHIFT];
			const uint8_t  filter    = 0 == cmpFunc ? 0 : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;

			D3D12_SAMPLER_DESC sd;
			sd.Filter   = (D3D12_FILTER)(filter|minFilter|magFilter|mipFilter);
			sd.AddressU = s_textureAddress[(flags&BGFX_TEXTURE_U_MASK)>>BGFX_TEXTURE_U_SHIFT];
			sd.AddressV = s_textureAddress[(flags&BGFX_TEXTURE_V_MASK)>>BGFX_TEXTURE_V_SHIFT];
			sd.AddressW = s_textureAddress[(flags&BGFX_TEXTURE_W_MASK)>>BGFX_TEXTURE_W_SHIFT];
			sd.MinLOD   = 0;
			sd.MaxLOD   = D3D12_FLOAT32_MAX;
			sd.MipLODBias     = 0.0f;
			sd.MaxAnisotropy  = 1; //m_maxAnisotropy;
			sd.ComparisonFunc = 0 == cmpFunc ? D3D12_COMPARISON_FUNC_NEVER : s_cmpFunc[cmpFunc];

			D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
			{
				m_cpuHandle.ptr + (idx * m_numDescriptorsPerBlock + ii) * m_incrementSize
			};

			device->CreateSampler(&sd, cpuHandle);
		}

		return idx;
	}

	void DescriptorAllocatorD3D12::free(uint16_t _idx)
	{
		m_handleAlloc->free(_idx);
	}

	void DescriptorAllocatorD3D12::reset()
	{
		uint16_t max = m_handleAlloc->getMaxHandles();
		bx::destroyHandleAlloc(g_allocator, m_handleAlloc);
		m_handleAlloc = bx::createHandleAlloc(g_allocator, max);
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocatorD3D12::get(uint16_t _idx)
	{
		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = { m_gpuHandle.ptr + _idx * m_numDescriptorsPerBlock * m_incrementSize };
		return gpuHandle;
	}

	void CommandQueueD3D12::init(ID3D12Device* _device)
	{
		D3D12_COMMAND_QUEUE_DESC queueDesc;
		queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Priority = 0;
		queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 1;
		DX_CHECK(_device->CreateCommandQueue(&queueDesc
				, IID_ID3D12CommandQueue
				, (void**)&m_commandQueue
				) );

		m_completedFence = 0;
		m_currentFence   = 0;
		DX_CHECK(_device->CreateFence(0
				, D3D12_FENCE_FLAG_NONE
				, IID_ID3D12Fence
				, (void**)&m_fence
				) );

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_commandList); ++ii)
		{
			DX_CHECK(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT
					, IID_ID3D12CommandAllocator
					, (void**)&m_commandList[ii].m_commandAllocator
					) );

			DX_CHECK(_device->CreateCommandList(0
					, D3D12_COMMAND_LIST_TYPE_DIRECT
					, m_commandList[ii].m_commandAllocator
					, NULL
					, IID_ID3D12GraphicsCommandList
					, (void**)&m_commandList[ii].m_commandList
					) );

			DX_CHECK(m_commandList[ii].m_commandList->Close() );
		}
	}

	void CommandQueueD3D12::shutdown()
	{
		finish(UINT64_MAX, true);

		DX_RELEASE(m_fence, 0);

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_commandList); ++ii)
		{
			DX_RELEASE(m_commandList[ii].m_commandAllocator, 0);
			DX_RELEASE(m_commandList[ii].m_commandList, 0);
		}

		DX_RELEASE(m_commandQueue, 0);
	}

	ID3D12GraphicsCommandList* CommandQueueD3D12::alloc()
	{
		while (0 == m_control.reserve(1) )
		{
			consume();
		}

		CommandList& commandList = m_commandList[m_control.m_current];
		DX_CHECK(commandList.m_commandAllocator->Reset() );
		DX_CHECK(commandList.m_commandList->Reset(commandList.m_commandAllocator, NULL) );
		return commandList.m_commandList;
	}

	uint64_t CommandQueueD3D12::kick()
	{
		CommandList& commandList = m_commandList[m_control.m_current];
		DX_CHECK(commandList.m_commandList->Close() );

		ID3D12CommandList* commandLists[] = { commandList.m_commandList };
		m_commandQueue->ExecuteCommandLists(BX_COUNTOF(commandLists), commandLists);

		commandList.m_event = CreateEventExA(NULL, NULL, 0, EVENT_ALL_ACCESS);
		const uint64_t fence = m_currentFence++;
		m_commandQueue->Signal(m_fence, fence);
		m_fence->SetEventOnCompletion(fence, commandList.m_event);

		m_control.commit(1);

		return fence;
	}

	void CommandQueueD3D12::finish(uint64_t _waitFence, bool _finishAll)
	{
		while (0 < m_control.available() )
		{
			consume();

			if (!_finishAll
			&&  _waitFence <= m_completedFence)
			{
				return;
			}
		}

		BX_CHECK(0 == m_control.available(), "");
	}

	bool CommandQueueD3D12::tryFinish(uint64_t _waitFence)
	{
		if (0 < m_control.available() )
		{
			if (consume(0)
			&& _waitFence <= m_completedFence)
			{
				return true;
			}
		}

		return false;
	}

	void CommandQueueD3D12::release(ID3D12Resource* _ptr)
	{
		m_release[m_control.m_current].push_back(_ptr);
	}

	bool CommandQueueD3D12::consume(uint32_t _ms)
	{
		CommandList& commandList = m_commandList[m_control.m_read];
		if (WAIT_OBJECT_0 == WaitForSingleObject(commandList.m_event, _ms) )
		{
			CloseHandle(commandList.m_event);
			commandList.m_event = NULL;
			m_completedFence = m_fence->GetCompletedValue();
			m_commandQueue->Wait(m_fence, m_completedFence);

			ResourceArray& ra = m_release[m_control.m_read];
			for (ResourceArray::iterator it = ra.begin(), itEnd = ra.end(); it != itEnd; ++it)
			{
				DX_RELEASE(*it, 0);
			}
			ra.clear();

			m_control.consume(1);

			return true;
		}

		return false;
	}

	void BatchD3D12::create(uint32_t _maxDrawPerBatch)
	{
		m_maxDrawPerBatch = _maxDrawPerBatch;
		setSeqMode(false);
		setIndirectMode(true);

		ID3D12Device* device = s_renderD3D12->m_device;
		ID3D12RootSignature* rootSignature = s_renderD3D12->m_rootSignature;

		D3D12_INDIRECT_ARGUMENT_DESC drawArgDesc[] =
		{
			{ D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, { Rdt::CBV } },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW,   0            },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW,   1            },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,                 0            },
		};

		D3D12_COMMAND_SIGNATURE_DESC drawCommandSignature =
		{
			sizeof(DrawIndirectCommand),
			BX_COUNTOF(drawArgDesc),
			drawArgDesc,
			1,
		};

		DX_CHECK(device->CreateCommandSignature(&drawCommandSignature
				, rootSignature
				, IID_ID3D12CommandSignature
				, (void**)&m_commandSignature[Draw]
				) );

		D3D12_INDIRECT_ARGUMENT_DESC drawIndexedArgDesc[] =
		{
			{ D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, { Rdt::CBV } },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW,   0            },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW,   1            },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW,    0            },
			{ D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,         0            },
		};

		D3D12_COMMAND_SIGNATURE_DESC drawIndexedCommandSignature =
		{
			sizeof(DrawIndexedIndirectCommand),
			BX_COUNTOF(drawIndexedArgDesc),
			drawIndexedArgDesc,
			1,
		};

		DX_CHECK(device->CreateCommandSignature(&drawIndexedCommandSignature
				, rootSignature
				, IID_ID3D12CommandSignature
				, (void**)&m_commandSignature[DrawIndexed]
				) );

		m_cmds[Draw       ] = BX_ALLOC(g_allocator, m_maxDrawPerBatch*sizeof(DrawIndirectCommand) );
		m_cmds[DrawIndexed] = BX_ALLOC(g_allocator, m_maxDrawPerBatch*sizeof(DrawIndexedIndirectCommand) );

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_indirect); ++ii)
		{
			m_indirect[ii].create(m_maxDrawPerBatch*sizeof(DrawIndexedIndirectCommand)
				, NULL
				, BGFX_BUFFER_DRAW_INDIRECT
				, false
				, sizeof(DrawIndexedIndirectCommand)
				);
		}
	}

	void BatchD3D12::destroy()
	{
		BX_FREE(g_allocator, m_cmds[0]);
		BX_FREE(g_allocator, m_cmds[1]);

		DX_RELEASE(m_commandSignature[0], 0);
		DX_RELEASE(m_commandSignature[1], 0);

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_indirect); ++ii)
		{
			m_indirect[ii].destroy();
		}
	}

	template<typename Ty>
	Ty& BatchD3D12::getCmd(Enum _type)
	{
		uint32_t index = m_num[_type];
		BX_CHECK(index < m_maxDrawPerBatch, "Memory corruption...");
		m_num[_type]++;
		Ty* cmd = &reinterpret_cast<Ty*>(m_cmds[_type])[index];
		return *cmd;
	}

	uint32_t BatchD3D12::draw(ID3D12GraphicsCommandList* _commandList, D3D12_GPU_VIRTUAL_ADDRESS _cbv, const RenderDraw& _draw)
	{
		Enum type = Enum(!!isValid(_draw.m_indexBuffer) );

		VertexBufferD3D12& vb = s_renderD3D12->m_vertexBuffers[_draw.m_vertexBuffer.idx];
		vb.setState(_commandList, D3D12_RESOURCE_STATE_GENERIC_READ);

		uint16_t declIdx = !isValid(vb.m_decl) ? _draw.m_vertexDecl.idx : vb.m_decl.idx;
		const VertexDecl& vertexDecl = s_renderD3D12->m_vertexDecls[declIdx];
		uint32_t numIndices = 0;

		if (Draw == type)
		{
			const uint32_t numVertices = UINT32_MAX == _draw.m_numVertices
				? vb.m_size / vertexDecl.m_stride
				: _draw.m_numVertices
				;

			DrawIndirectCommand& cmd = getCmd<DrawIndirectCommand>(Draw);
			cmd.cbv = _cbv;
			cmd.vbv[0].BufferLocation = vb.m_gpuVA;
			cmd.vbv[0].StrideInBytes  = vertexDecl.m_stride;
			cmd.vbv[0].SizeInBytes    = vb.m_size;
			if (isValid(_draw.m_instanceDataBuffer) )
			{
				const VertexBufferD3D12& inst = s_renderD3D12->m_vertexBuffers[_draw.m_instanceDataBuffer.idx];
				cmd.vbv[1].BufferLocation = inst.m_gpuVA + _draw.m_instanceDataOffset;
				cmd.vbv[1].StrideInBytes  = _draw.m_instanceDataStride;
				cmd.vbv[1].SizeInBytes    = _draw.m_numInstances * _draw.m_instanceDataStride;
			}
			else
			{
				memset(&cmd.vbv[1], 0, sizeof(cmd.vbv[1]) );
			}
			cmd.draw.InstanceCount = _draw.m_numInstances;
			cmd.draw.VertexCountPerInstance = numVertices;
			cmd.draw.StartVertexLocation    = _draw.m_startVertex;
			cmd.draw.StartInstanceLocation  = 0;
		}
		else
		{
			BufferD3D12& ib = s_renderD3D12->m_indexBuffers[_draw.m_indexBuffer.idx];
			ib.setState(_commandList, D3D12_RESOURCE_STATE_GENERIC_READ);

			const bool hasIndex16 = 0 == (ib.m_flags & BGFX_BUFFER_INDEX32);
			const uint32_t indexSize = hasIndex16 ? 2 : 4;

			numIndices = UINT32_MAX == _draw.m_numIndices
				? ib.m_size / indexSize
				: _draw.m_numIndices
				;

			DrawIndexedIndirectCommand& cmd = getCmd<DrawIndexedIndirectCommand>(DrawIndexed);
			cmd.cbv = _cbv;
			cmd.ibv.BufferLocation = ib.m_gpuVA;
			cmd.ibv.SizeInBytes    = ib.m_size;
			cmd.ibv.Format = hasIndex16
				? DXGI_FORMAT_R16_UINT
				: DXGI_FORMAT_R32_UINT
				;
			cmd.vbv[0].BufferLocation = vb.m_gpuVA;
			cmd.vbv[0].StrideInBytes  = vertexDecl.m_stride;
			cmd.vbv[0].SizeInBytes    = vb.m_size;
			if (isValid(_draw.m_instanceDataBuffer) )
			{
				const VertexBufferD3D12& inst = s_renderD3D12->m_vertexBuffers[_draw.m_instanceDataBuffer.idx];
				cmd.vbv[1].BufferLocation = inst.m_gpuVA + _draw.m_instanceDataOffset;
				cmd.vbv[1].StrideInBytes  = _draw.m_instanceDataStride;
				cmd.vbv[1].SizeInBytes    = _draw.m_numInstances * _draw.m_instanceDataStride;
			}
			else
			{
				memset(&cmd.vbv[1], 0, sizeof(cmd.vbv[1]) );
			}
			cmd.drawIndexed.IndexCountPerInstance = numIndices;
			cmd.drawIndexed.InstanceCount = _draw.m_numInstances;
			cmd.drawIndexed.StartIndexLocation = _draw.m_startIndex;
			cmd.drawIndexed.BaseVertexLocation = _draw.m_startVertex;
			cmd.drawIndexed.StartInstanceLocation = 0;
		}

		if (BX_UNLIKELY(m_flushPerBatch == m_num[type]) )
		{
			flush(_commandList, type);
		}

		return numIndices;
	}

	static const uint32_t s_indirectCommandSize[] =
	{
		sizeof(BatchD3D12::DrawIndirectCommand),
		sizeof(BatchD3D12::DrawIndexedIndirectCommand),
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_indirectCommandSize) == BatchD3D12::Count);

	void BatchD3D12::flush(ID3D12GraphicsCommandList* _commandList, Enum _type)
	{
		uint32_t num = m_num[_type];
		if (0 != num)
		{
			m_num[_type] = 0;

			if (m_minIndirect < num)
			{
				m_stats.m_numIndirect[_type]++;

				BufferD3D12& indirect = m_indirect[m_currIndirect++];
				m_currIndirect %= BX_COUNTOF(m_indirect);

				indirect.update(_commandList, 0, num*s_indirectCommandSize[_type], m_cmds[_type]);

				_commandList->ExecuteIndirect(m_commandSignature[_type]
					, num
					, indirect.m_ptr
					, 0
					, NULL
					, 0
					);
			}
			else
			{
				m_stats.m_numImmediate[_type]++;

				if (Draw == _type)
				{
					const DrawIndirectCommand* cmds = reinterpret_cast<DrawIndirectCommand*>(m_cmds[_type]);

					for (uint32_t ii = 0; ii < num; ++ii)
					{
						const DrawIndirectCommand& cmd = cmds[ii];
						if (m_current.cbv != cmd.cbv)
						{
							m_current.cbv = cmd.cbv;
							_commandList->SetGraphicsRootConstantBufferView(Rdt::CBV, cmd.cbv);
						}

						if (0 != memcmp(m_current.vbv, cmd.vbv, sizeof(cmd.vbv) ) )
						{
							memcpy(m_current.vbv, cmd.vbv, sizeof(cmd.vbv) );
							_commandList->IASetVertexBuffers(0
								, 0 == cmd.vbv[1].BufferLocation ? 1 : 2
								, cmd.vbv
								);
						}

						_commandList->DrawInstanced(
							  cmd.draw.VertexCountPerInstance
							, cmd.draw.InstanceCount
							, cmd.draw.StartVertexLocation
							, cmd.draw.StartInstanceLocation
							);
					}
				}
				else
				{
					const DrawIndexedIndirectCommand* cmds = reinterpret_cast<DrawIndexedIndirectCommand*>(m_cmds[_type]);

					for (uint32_t ii = 0; ii < num; ++ii)
					{
						const DrawIndexedIndirectCommand& cmd = cmds[ii];
						if (m_current.cbv != cmd.cbv)
						{
							m_current.cbv = cmd.cbv;
							_commandList->SetGraphicsRootConstantBufferView(Rdt::CBV, cmd.cbv);
						}

						if (0 != memcmp(m_current.vbv, cmd.vbv, sizeof(cmd.vbv) ) )
						{
							memcpy(m_current.vbv, cmd.vbv, sizeof(cmd.vbv) );
							_commandList->IASetVertexBuffers(0
								, 0 == cmd.vbv[1].BufferLocation ? 1 : 2
								, cmd.vbv
								);
						}

						if (0 != memcmp(&m_current.ibv, &cmd.ibv, sizeof(cmd.ibv) ) )
						{
							memcpy(&m_current.ibv, &cmd.ibv, sizeof(cmd.ibv) );
							_commandList->IASetIndexBuffer(&cmd.ibv);
						}

						_commandList->DrawIndexedInstanced(
							  cmd.drawIndexed.IndexCountPerInstance
							, cmd.drawIndexed.InstanceCount
							, cmd.drawIndexed.StartIndexLocation
							, cmd.drawIndexed.BaseVertexLocation
							, cmd.drawIndexed.StartInstanceLocation
							);
					}
				}
			}
		}
	}

	void BatchD3D12::flush(ID3D12GraphicsCommandList* _commandList, bool _clean)
	{
		flush(_commandList, Draw);
		flush(_commandList, DrawIndexed);

		if (_clean)
		{
			memset(&m_current, 0, sizeof(m_current) );
		}
	}

	void BatchD3D12::begin()
	{
		memset(&m_stats,   0, sizeof(m_stats) );
		memset(&m_current, 0, sizeof(m_current) );
	}

	void BatchD3D12::end(ID3D12GraphicsCommandList* _commandList)
	{
		flush(_commandList);
	}

	struct UavFormat
	{
		DXGI_FORMAT format[3];
		uint32_t    stride;
	};

	static const UavFormat s_uavFormat[] =
	{	//  BGFX_BUFFER_COMPUTE_TYPE_UINT, BGFX_BUFFER_COMPUTE_TYPE_INT,   BGFX_BUFFER_COMPUTE_TYPE_FLOAT
		{ { DXGI_FORMAT_UNKNOWN,           DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN            },  0 }, // ignored
		{ { DXGI_FORMAT_R8_SINT,           DXGI_FORMAT_R8_UINT,            DXGI_FORMAT_UNKNOWN            },  1 }, // BGFX_BUFFER_COMPUTE_FORMAT_8x1
		{ { DXGI_FORMAT_R8G8_SINT,         DXGI_FORMAT_R8G8_UINT,          DXGI_FORMAT_UNKNOWN            },  2 }, // BGFX_BUFFER_COMPUTE_FORMAT_8x2
		{ { DXGI_FORMAT_R8G8B8A8_SINT,     DXGI_FORMAT_R8G8B8A8_UINT,      DXGI_FORMAT_UNKNOWN            },  4 }, // BGFX_BUFFER_COMPUTE_FORMAT_8x4
		{ { DXGI_FORMAT_R16_SINT,          DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_R16_FLOAT          },  2 }, // BGFX_BUFFER_COMPUTE_FORMAT_16x1
		{ { DXGI_FORMAT_R16G16_SINT,       DXGI_FORMAT_R16G16_UINT,        DXGI_FORMAT_R16G16_FLOAT       },  4 }, // BGFX_BUFFER_COMPUTE_FORMAT_16x2
		{ { DXGI_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_UINT,  DXGI_FORMAT_R16G16B16A16_FLOAT },  8 }, // BGFX_BUFFER_COMPUTE_FORMAT_16x4
		{ { DXGI_FORMAT_R32_SINT,          DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_FLOAT          },  4 }, // BGFX_BUFFER_COMPUTE_FORMAT_32x1
		{ { DXGI_FORMAT_R32G32_SINT,       DXGI_FORMAT_R32G32_UINT,        DXGI_FORMAT_R32G32_FLOAT       },  8 }, // BGFX_BUFFER_COMPUTE_FORMAT_32x2
		{ { DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_UINT,  DXGI_FORMAT_R32G32B32A32_FLOAT }, 16 }, // BGFX_BUFFER_COMPUTE_FORMAT_32x4
	};

	void BufferD3D12::create(uint32_t _size, void* _data, uint16_t _flags, bool _vertex, uint32_t _stride)
	{
		m_size    = _size;
		m_flags   = _flags;

		const bool needUav = 0 != (_flags & (BGFX_BUFFER_COMPUTE_WRITE|BGFX_BUFFER_DRAW_INDIRECT) );
		const bool drawIndirect = 0 != (_flags & BGFX_BUFFER_DRAW_INDIRECT);
		m_dynamic = NULL == _data || needUav;

		DXGI_FORMAT format;
		uint32_t    stride;

		D3D12_RESOURCE_FLAGS flags = needUav
			? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
			: D3D12_RESOURCE_FLAG_NONE
			;

		if (drawIndirect)
		{
			format = DXGI_FORMAT_R32G32B32A32_UINT;
			stride = 16;
		}
		else
		{
			uint32_t uavFormat = (_flags & BGFX_BUFFER_COMPUTE_FORMAT_MASK) >> BGFX_BUFFER_COMPUTE_FORMAT_SHIFT;
			if (0 == uavFormat)
			{
				if (_vertex)
				{
					format = DXGI_FORMAT_R32G32B32A32_FLOAT;
					stride = 16;
				}
				else
				{
					if (0 == (_flags & BGFX_BUFFER_INDEX32) )
					{
						format = DXGI_FORMAT_R16_UINT;
						stride = 2;
					}
					else
					{
						format = DXGI_FORMAT_R32_UINT;
						stride = 4;
					}
				}
			}
			else
			{
				const uint32_t uavType = bx::uint32_satsub( (_flags & BGFX_BUFFER_COMPUTE_TYPE_MASK) >> BGFX_BUFFER_COMPUTE_TYPE_SHIFT, 1);
				format = s_uavFormat[uavFormat].format[uavType];
				stride = s_uavFormat[uavFormat].stride;
			}
		}

		stride = 0 == _stride ? stride : _stride;

		m_srvd.Format                      = format;
		m_srvd.ViewDimension               = D3D12_SRV_DIMENSION_BUFFER;
		m_srvd.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		m_srvd.Buffer.FirstElement         = 0;
		m_srvd.Buffer.NumElements          = m_size / stride;
		m_srvd.Buffer.StructureByteStride  = 0;
		m_srvd.Buffer.Flags                = D3D12_BUFFER_SRV_FLAG_NONE;

		m_uavd.Format                      = format;
		m_uavd.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
		m_uavd.Buffer.FirstElement         = 0;
		m_uavd.Buffer.NumElements          = m_size / stride;
		m_uavd.Buffer.StructureByteStride  = 0;
		m_uavd.Buffer.CounterOffsetInBytes = 0;
		m_uavd.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_NONE;

		ID3D12Device* device = s_renderD3D12->m_device;
		ID3D12GraphicsCommandList* commandList = s_renderD3D12->m_commandList;

		m_ptr   = createCommittedResource(device, HeapProperty::Default, _size, flags);
		m_gpuVA = m_ptr->GetGPUVirtualAddress();
		setState(commandList, drawIndirect
			? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
			: D3D12_RESOURCE_STATE_GENERIC_READ
			);

		if (!m_dynamic)
		{
			update(commandList, 0, _size, _data);
		}
	}

	void BufferD3D12::update(ID3D12GraphicsCommandList* _commandList, uint32_t _offset, uint32_t _size, void* _data, bool /*_discard*/)
	{
		ID3D12Resource* staging = createCommittedResource(s_renderD3D12->m_device, HeapProperty::Upload, _size);
		uint8_t* data;
		DX_CHECK(staging->Map(0, NULL, (void**)&data) );
		memcpy(data, _data, _size);
		staging->Unmap(0, NULL);

		D3D12_RESOURCE_STATES state = setState(_commandList, D3D12_RESOURCE_STATE_COPY_DEST);
		_commandList->CopyBufferRegion(m_ptr, _offset, staging, 0, _size);
		setState(_commandList, state);

		s_renderD3D12->m_cmd.release(staging);
	}

	void BufferD3D12::destroy()
	{
		if (NULL != m_ptr)
		{
			s_renderD3D12->m_cmd.release(m_ptr);
			m_dynamic = false;
		}
	}

	D3D12_RESOURCE_STATES BufferD3D12::setState(ID3D12GraphicsCommandList* _commandList, D3D12_RESOURCE_STATES _state)
	{
		if (m_state != _state)
		{
			setResourceBarrier(_commandList
				, m_ptr
				, m_state
				, _state
				);

			bx::xchg(m_state, _state);
		}

		return _state;
	}

	void VertexBufferD3D12::create(uint32_t _size, void* _data, VertexDeclHandle _declHandle, uint16_t _flags)
	{
		BufferD3D12::create(_size, _data, _flags, true);
		m_decl = _declHandle;
	}

	void ShaderD3D12::create(const Memory* _mem)
	{
		bx::MemoryReader reader(_mem->data, _mem->size);

		uint32_t magic;
		bx::read(&reader, magic);

		switch (magic)
		{
		case BGFX_CHUNK_MAGIC_CSH:
		case BGFX_CHUNK_MAGIC_FSH:
		case BGFX_CHUNK_MAGIC_VSH:
			break;

		default:
			BGFX_FATAL(false, Fatal::InvalidShader, "Unknown shader format %x.", magic);
			break;
		}

		bool fragment = BGFX_CHUNK_MAGIC_FSH == magic;

		uint32_t iohash;
		bx::read(&reader, iohash);

		uint16_t count;
		bx::read(&reader, count);

		m_numPredefined = 0;
		m_numUniforms = count;

		BX_TRACE("%s Shader consts %d"
			, BGFX_CHUNK_MAGIC_FSH == magic ? "Fragment" : BGFX_CHUNK_MAGIC_VSH == magic ? "Vertex" : "Compute"
			, count
			);

		uint8_t fragmentBit = fragment ? BGFX_UNIFORM_FRAGMENTBIT : 0;

		if (0 < count)
		{
			for (uint32_t ii = 0; ii < count; ++ii)
			{
				uint8_t nameSize;
				bx::read(&reader, nameSize);

				char name[256];
				bx::read(&reader, &name, nameSize);
				name[nameSize] = '\0';

				uint8_t type;
				bx::read(&reader, type);

				uint8_t num;
				bx::read(&reader, num);

				uint16_t regIndex;
				bx::read(&reader, regIndex);

				uint16_t regCount;
				bx::read(&reader, regCount);

				const char* kind = "invalid";

				PredefinedUniform::Enum predefined = nameToPredefinedUniformEnum(name);
				if (PredefinedUniform::Count != predefined)
				{
					kind = "predefined";
					m_predefined[m_numPredefined].m_loc   = regIndex;
					m_predefined[m_numPredefined].m_count = regCount;
					m_predefined[m_numPredefined].m_type  = uint8_t(predefined|fragmentBit);
					m_numPredefined++;
				}
				else
				{
					const UniformInfo* info = s_renderD3D12->m_uniformReg.find(name);

					if (NULL != info)
					{
						if (NULL == m_constantBuffer)
						{
							m_constantBuffer = ConstantBuffer::create(1024);
						}

						kind = "user";
						m_constantBuffer->writeUniformHandle( (UniformType::Enum)(type|fragmentBit), regIndex, info->m_handle, regCount);
					}
				}

				BX_TRACE("\t%s: %s (%s), num %2d, r.index %3d, r.count %2d"
					, kind
					, name
					, getUniformTypeName(UniformType::Enum(type&~BGFX_UNIFORM_FRAGMENTBIT) )
					, num
					, regIndex
					, regCount
					);
				BX_UNUSED(kind);
			}

			if (NULL != m_constantBuffer)
			{
				m_constantBuffer->finish();
			}
		}

		uint16_t shaderSize;
		bx::read(&reader, shaderSize);

		const DWORD* code = (const DWORD*)reader.getDataPtr();
		bx::skip(&reader, shaderSize+1);

		m_code = copy(code, shaderSize);

		uint8_t numAttrs;
		bx::read(&reader, numAttrs);

		memset(m_attrMask, 0, sizeof(m_attrMask) );

		for (uint32_t ii = 0; ii < numAttrs; ++ii)
		{
			uint16_t id;
			bx::read(&reader, id);

			Attrib::Enum attr = idToAttrib(id);

			if (Attrib::Count != attr)
			{
				m_attrMask[attr] = UINT16_MAX;
			}
		}

		bx::HashMurmur2A murmur;
		murmur.begin();
		murmur.add(iohash);
		murmur.add(code, shaderSize);
		murmur.add(numAttrs);
		murmur.add(m_attrMask, numAttrs);
		m_hash = murmur.end();

		bx::read(&reader, m_size);
	}

	void TextureD3D12::create(const Memory* _mem, uint32_t _flags, uint8_t _skip)
	{
		ImageContainer imageContainer;

		if (imageParse(imageContainer, _mem->data, _mem->size) )
		{
			uint8_t numMips = imageContainer.m_numMips;
			const uint8_t startLod = uint8_t(bx::uint32_min(_skip, numMips-1) );
			numMips -= startLod;
			const ImageBlockInfo& blockInfo = getBlockInfo(TextureFormat::Enum(imageContainer.m_format) );
			const uint32_t textureWidth  = bx::uint32_max(blockInfo.blockWidth,  imageContainer.m_width >>startLod);
			const uint32_t textureHeight = bx::uint32_max(blockInfo.blockHeight, imageContainer.m_height>>startLod);

			m_flags = _flags;
			m_requestedFormat = (uint8_t)imageContainer.m_format;
			m_textureFormat   = (uint8_t)imageContainer.m_format;

			const TextureFormatInfo& tfi = s_textureFormat[m_requestedFormat];
			const bool convert = DXGI_FORMAT_UNKNOWN == tfi.m_fmt;

			uint8_t bpp = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
			if (convert)
			{
				m_textureFormat = (uint8_t)TextureFormat::BGRA8;
				bpp = 32;
			}

			if (imageContainer.m_cubeMap)
			{
				m_type = TextureCube;
			}
			else if (imageContainer.m_depth > 1)
			{
				m_type = Texture3D;
			}
			else
			{
				m_type = Texture2D;
			}

			m_numMips = numMips;
			const uint16_t numSides = imageContainer.m_cubeMap ? 6 : 1;

			uint32_t numSrd = numMips*numSides;
			D3D12_SUBRESOURCE_DATA* srd = (D3D12_SUBRESOURCE_DATA*)alloca(numSrd*sizeof(D3D12_SUBRESOURCE_DATA) );

			uint32_t kk = 0;

			const bool compressed = isCompressed(TextureFormat::Enum(m_textureFormat) );
			const bool swizzle    = TextureFormat::BGRA8 == m_textureFormat && 0 != (m_flags&BGFX_TEXTURE_COMPUTE_WRITE);
			uint32_t blockWidth   = 1;
			uint32_t blockHeight  = 1;

			if (convert && compressed)
			{
				blockWidth  = blockInfo.blockWidth;
				blockHeight = blockInfo.blockHeight;
			}

			const bool bufferOnly   = 0 != (m_flags&BGFX_TEXTURE_RT_BUFFER_ONLY);
			const bool computeWrite = 0 != (m_flags&BGFX_TEXTURE_COMPUTE_WRITE);
			const bool renderTarget = 0 != (m_flags&BGFX_TEXTURE_RT_MASK);

			BX_TRACE("Texture %3d: %s (requested: %s), %dx%d%s RT[%c], BO[%c], CW[%c]%s."
				, this - s_renderD3D12->m_textures
				, getName( (TextureFormat::Enum)m_textureFormat)
				, getName( (TextureFormat::Enum)m_requestedFormat)
				, textureWidth
				, textureHeight
				, imageContainer.m_cubeMap ? "x6" : ""
				, renderTarget ? 'x' : ' '
				, bufferOnly   ? 'x' : ' '
				, computeWrite ? 'x' : ' '
				, swizzle ? " (swizzle BGRA8 -> RGBA8)" : ""
				);

			uint32_t totalSize = 0;

			for (uint8_t side = 0; side < numSides; ++side)
			{
				uint32_t width  = textureWidth;
				uint32_t height = textureHeight;
				uint32_t depth  = imageContainer.m_depth;

				for (uint8_t lod = 0; lod < numMips; ++lod)
				{
					width  = bx::uint32_max(blockWidth,  width);
					height = bx::uint32_max(blockHeight, height);
					depth  = bx::uint32_max(1, depth);

					ImageMip mip;
					if (imageGetRawData(imageContainer, side, lod+startLod, _mem->data, _mem->size, mip) )
					{
						if (convert)
						{
							const uint32_t pitch = bx::strideAlign(width*bpp / 8,  D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
							const uint32_t slice = bx::strideAlign(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

							uint8_t* temp = (uint8_t*)BX_ALLOC(g_allocator, slice);
							imageDecodeToBgra8(temp
									, mip.m_data
									, mip.m_width
									, mip.m_height
									, pitch, mip.m_format
									);

							srd[kk].pData      = temp;
							srd[kk].RowPitch   = pitch;
							srd[kk].SlicePitch = slice;
							totalSize += slice;
						}
						else if (compressed)
						{
							uint32_t pitch = bx::strideAlign( (mip.m_width /blockInfo.blockWidth )*mip.m_blockSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
							uint32_t slice = bx::strideAlign( (mip.m_height/blockInfo.blockHeight)*pitch,           D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

							uint8_t* temp = (uint8_t*)BX_ALLOC(g_allocator, slice);
							imageCopy(mip.m_height/blockInfo.blockHeight
									, (mip.m_width /blockInfo.blockWidth )*mip.m_blockSize
									, mip.m_data
									, pitch
									, temp
									);

							srd[kk].pData      = temp;
							srd[kk].RowPitch   = pitch;
							srd[kk].SlicePitch = slice;
							totalSize += slice;
						}
						else
						{
							const uint32_t pitch = bx::strideAlign(mip.m_width*mip.m_bpp / 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
							const uint32_t slice = bx::strideAlign(pitch * mip.m_height,      D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

							uint8_t* temp = (uint8_t*)BX_ALLOC(g_allocator, slice);
							imageCopy(mip.m_height
									, mip.m_width*mip.m_bpp / 8
									, mip.m_data
									, pitch
									, temp
									);

							srd[kk].pData = temp;
							srd[kk].RowPitch   = pitch;
							srd[kk].SlicePitch = slice;
							totalSize += slice;
						}

 						if (swizzle)
 						{
// 							imageSwizzleBgra8(width, height, mip.m_width*4, data, temp);
 						}

						srd[kk].SlicePitch = mip.m_height*srd[kk].RowPitch;
						++kk;
					}
					else
					{
						const uint32_t pitch = bx::strideAlign(width*bpp / 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
						const uint32_t slice = bx::strideAlign(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
						totalSize += slice;
					}

					width  >>= 1;
					height >>= 1;
					depth  >>= 1;
				}
			}

			BX_TRACE("texture total size: %d", totalSize);

			const uint32_t msaaQuality = bx::uint32_satsub( (m_flags&BGFX_TEXTURE_RT_MSAA_MASK)>>BGFX_TEXTURE_RT_MSAA_SHIFT, 1);
			const DXGI_SAMPLE_DESC& msaa = s_msaa[msaaQuality];

			memset(&m_srvd, 0, sizeof(m_srvd) );
			m_srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			m_srvd.Format = s_textureFormat[m_textureFormat].m_fmtSrv;
			DXGI_FORMAT format = s_textureFormat[m_textureFormat].m_fmt;
			if (swizzle)
			{
				format        = DXGI_FORMAT_R8G8B8A8_UNORM;
				m_srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			}

			m_uavd.Format = m_srvd.Format;

			ID3D12Device* device = s_renderD3D12->m_device;
			ID3D12GraphicsCommandList* commandList = s_renderD3D12->m_commandList;

			D3D12_RESOURCE_DESC resourceDesc;
			resourceDesc.Alignment  = 0;
			resourceDesc.Width      = textureWidth;
			resourceDesc.Height     = textureHeight;
			resourceDesc.MipLevels  = numMips;
			resourceDesc.Format     = format;
			resourceDesc.SampleDesc = msaa;
			resourceDesc.Layout     = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			resourceDesc.Flags      = D3D12_RESOURCE_FLAG_NONE;
			resourceDesc.DepthOrArraySize = numSides;

			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

			D3D12_CLEAR_VALUE* clearValue = NULL;
			if (isDepth(TextureFormat::Enum(m_textureFormat) ) )
			{
				resourceDesc.Format = s_textureFormat[m_textureFormat].m_fmt;
				resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
				state              |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
				state              &= ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

				clearValue = (D3D12_CLEAR_VALUE*)alloca(sizeof(D3D12_CLEAR_VALUE) );
				clearValue->Format = s_textureFormat[m_textureFormat].m_fmtDsv;
				clearValue->DepthStencil.Depth   = 1.0f;
				clearValue->DepthStencil.Stencil = 0;
			}
			else if (renderTarget)
			{
				clearValue = (D3D12_CLEAR_VALUE*)alloca(sizeof(D3D12_CLEAR_VALUE) );
				clearValue->Format = resourceDesc.Format;
				clearValue->Color[0] = 0.0f;
				clearValue->Color[1] = 0.0f;
				clearValue->Color[2] = 0.0f;
				clearValue->Color[3] = 0.0f;

				resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			}

			if (bufferOnly)
			{
				resourceDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
				state              &= ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			}

			if (computeWrite)
			{
				resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			}

			switch (m_type)
			{
			case Texture2D:
				resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				m_srvd.ViewDimension                 = 1 < msaa.Count ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
				m_srvd.Texture2D.MostDetailedMip     = 0;
				m_srvd.Texture2D.MipLevels           = numMips;
				m_srvd.Texture2D.ResourceMinLODClamp = 0.0f;

				m_uavd.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
				m_uavd.Texture2D.MipSlice   = 0;
				m_uavd.Texture2D.PlaneSlice = 0;
				break;

			case Texture3D:
				resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
				m_srvd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE3D;
				m_srvd.Texture3D.MostDetailedMip     = 0;
				m_srvd.Texture3D.MipLevels           = numMips;
				m_srvd.Texture3D.ResourceMinLODClamp = 0.0f;

				m_uavd.ViewDimension         = D3D12_UAV_DIMENSION_TEXTURE3D;
				m_uavd.Texture3D.MipSlice    = 0;
				m_uavd.Texture3D.FirstWSlice = 0;
				m_uavd.Texture3D.WSize       = 0;
				break;

			case TextureCube:
				resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				m_srvd.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
				m_srvd.TextureCube.MostDetailedMip     = 0;
				m_srvd.TextureCube.MipLevels           = numMips;
				m_srvd.TextureCube.ResourceMinLODClamp = 0.0f;

				m_uavd.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
				m_uavd.Texture2D.MipSlice   = 0;
				m_uavd.Texture2D.PlaneSlice = 0;
				break;
			}

			m_ptr = createCommittedResource(device, HeapProperty::Default, &resourceDesc, clearValue);

			{
				uint64_t uploadBufferSize;
				uint32_t* numRows        = (uint32_t*)alloca(sizeof(uint32_t)*numSrd);
				uint64_t* rowSizeInBytes = (uint64_t*)alloca(sizeof(uint64_t)*numSrd);
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*)alloca(sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT)*numSrd);

				device->GetCopyableFootprints(&resourceDesc
					, 0
					, numSrd
					, 0
					, layouts
					, numRows
					, rowSizeInBytes
					, &uploadBufferSize
					);
				BX_WARN(uploadBufferSize == totalSize, "uploadBufferSize %d (totalSize %d), numRows %d, rowSizeInBytes %d"
					, uploadBufferSize
					, totalSize
					, numRows[0]
					, rowSizeInBytes[0]
					);
			}

			if (kk != 0)
			{
				ID3D12Resource* staging = createCommittedResource(s_renderD3D12->m_device, HeapProperty::Upload, totalSize);

				setState(commandList,D3D12_RESOURCE_STATE_COPY_DEST);

				uint64_t result = UpdateSubresources(commandList
					, m_ptr
					, staging
					, 0
					, 0
					, numSrd
					, srd
					);
				BX_CHECK(0 != result, "Invalid size"); BX_UNUSED(result);
				BX_TRACE("Update subresource %" PRId64, result);

				setState(commandList, state);

				s_renderD3D12->m_cmd.release(staging);
			}
			else
			{
				setState(commandList, state);
			}

			if (0 != kk)
			{
				kk = 0;
				for (uint8_t side = 0; side < numSides; ++side)
				{
					for (uint32_t lod = 0, num = numMips; lod < num; ++lod)
					{
						BX_FREE(g_allocator, const_cast<void*>(srd[kk].pData) );
						++kk;
					}
				}
			}
		}
	}

	void TextureD3D12::destroy()
	{
		if (NULL != m_ptr)
		{
			s_renderD3D12->m_cmd.release(m_ptr);
			m_ptr = NULL;
		}
	}

	void TextureD3D12::update(ID3D12GraphicsCommandList* _commandList, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem)
	{
		setState(_commandList, D3D12_RESOURCE_STATE_COPY_DEST);

		const uint32_t subres = _mip + (_side * m_numMips);
		const uint32_t bpp    = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
		const uint32_t rectpitch = _rect.m_width*bpp/8;
		const uint32_t srcpitch  = UINT16_MAX == _pitch ? rectpitch : _pitch;

		D3D12_RESOURCE_DESC desc = m_ptr->GetDesc();

		desc.Height = _rect.m_height;

		uint32_t numRows;
		uint64_t rowPitch;
		uint64_t totalBytes;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
		s_renderD3D12->m_device->GetCopyableFootprints(&desc
			, subres
			, 1
			, 0
			, &layout
			, &numRows
			, &rowPitch
			, &totalBytes
			);

		ID3D12Resource* staging = createCommittedResource(s_renderD3D12->m_device, HeapProperty::Upload, totalBytes);

		uint8_t* data;

		DX_CHECK(staging->Map(0, NULL, (void**)&data) );
		for (uint32_t ii = 0, height = _rect.m_height; ii < height; ++ii)
		{
			memcpy(&data[ii*rowPitch], &_mem->data[ii*srcpitch], srcpitch);
		}
		staging->Unmap(0, NULL);

		D3D12_BOX box;
		box.left   = 0;
		box.top    = 0;
		box.right  = box.left + _rect.m_width;
		box.bottom = box.top  + _rect.m_height;
		box.front  = _z;
		box.back   = _z+_depth;

		D3D12_TEXTURE_COPY_LOCATION dst = { m_ptr,   D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}     };
		dst.SubresourceIndex = subres;
		D3D12_TEXTURE_COPY_LOCATION src = { staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,  layout };
		_commandList->CopyTextureRegion(&dst, _rect.m_x, _rect.m_y, 0, &src, &box);

		setState(_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		s_renderD3D12->m_cmd.release(staging);
	}

	void TextureD3D12::resolve()
	{
	}

	D3D12_RESOURCE_STATES TextureD3D12::setState(ID3D12GraphicsCommandList* _commandList, D3D12_RESOURCE_STATES _state)
	{
		if (m_state != _state)
		{
			setResourceBarrier(_commandList
				, m_ptr
				, m_state
				, _state
				);

			bx::xchg(m_state, _state);
		}

		return _state;
	}

	void FrameBufferD3D12::create(uint8_t _num, const TextureHandle* _handles)
	{
		m_numTh = _num;
		memcpy(m_th, _handles, _num*sizeof(TextureHandle) );

		postReset();
	}

	void FrameBufferD3D12::create(uint16_t /*_denseIdx*/, void* /*_nwh*/, uint32_t /*_width*/, uint32_t /*_height*/, TextureFormat::Enum /*_depthFormat*/)
	{
	}

	void FrameBufferD3D12::preReset()
	{
	}

	void FrameBufferD3D12::postReset()
	{
		if (m_numTh != 0)
		{
			ID3D12Device* device = s_renderD3D12->m_device;

			D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor = s_renderD3D12->m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
			uint32_t rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			uint32_t fbhIdx = (uint32_t)(this - s_renderD3D12->m_frameBuffers);
			rtvDescriptor.ptr += (BX_COUNTOF(s_renderD3D12->m_backBufferColor) + fbhIdx * BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) * rtvDescriptorSize;

			m_depth.idx = bgfx::invalidHandle;
			m_num = 0;
			for (uint32_t ii = 0; ii < m_numTh; ++ii)
			{
				TextureHandle handle = m_th[ii];
				if (isValid(handle) )
				{
					const TextureD3D12& texture = s_renderD3D12->m_textures[handle.idx];
					if (isDepth( (TextureFormat::Enum)texture.m_textureFormat) )
					{
						BX_CHECK(!isValid(m_depth), "");
						m_depth = handle;
						D3D12_CPU_DESCRIPTOR_HANDLE dsvDescriptor = s_renderD3D12->m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
						uint32_t dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
						dsvDescriptor.ptr += (1 + fbhIdx) * dsvDescriptorSize;

						const ImageBlockInfo& blockInfo = getBlockInfo(TextureFormat::Enum(texture.m_textureFormat) );
						BX_UNUSED(blockInfo);

						D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
						ZeroMemory(&dsvDesc, sizeof(dsvDesc) );
						dsvDesc.Format        = s_textureFormat[texture.m_textureFormat].m_fmtDsv;
						dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
						dsvDesc.Flags         = D3D12_DSV_FLAG_NONE
// 							| (blockInfo.depthBits   > 0 ? D3D12_DSV_FLAG_READ_ONLY_DEPTH   : D3D12_DSV_FLAG_NONE)
// 							| (blockInfo.stencilBits > 0 ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_NONE)
							;

						device->CreateDepthStencilView(texture.m_ptr
							, &dsvDesc
							, dsvDescriptor
							);
					}
					else
					{
						m_texture[m_num] = handle;
						D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtvDescriptor.ptr + m_num * rtvDescriptorSize };
						device->CreateRenderTargetView(texture.m_ptr
							, NULL
							, rtv
							);
						m_num++;
					}
				}
			}
		}
	}

	uint16_t FrameBufferD3D12::destroy()
	{
		m_numTh = 0;

		m_depth.idx = bgfx::invalidHandle;

		uint16_t denseIdx = m_denseIdx;
		m_denseIdx = UINT16_MAX;

		return denseIdx;
	}

	void FrameBufferD3D12::resolve()
	{
	}

	void FrameBufferD3D12::clear(ID3D12GraphicsCommandList* _commandList, const Clear& _clear, const float _palette[][4], const D3D12_RECT* _rect, uint32_t _num)
	{
		ID3D12Device* device = s_renderD3D12->m_device;
		const uint32_t fbhIdx = (uint32_t)(this - s_renderD3D12->m_frameBuffers);

		if (BGFX_CLEAR_COLOR & _clear.m_flags
		&&  0 != m_num)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor = s_renderD3D12->m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
			uint32_t rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			rtvDescriptor.ptr += (BX_COUNTOF(s_renderD3D12->m_backBufferColor) + fbhIdx * BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) * rtvDescriptorSize;

			if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
			{
				for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
				{
					uint8_t index = _clear.m_index[ii];
					if (UINT8_MAX != index)
					{
						D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtvDescriptor.ptr + ii * rtvDescriptorSize };
						_commandList->ClearRenderTargetView(rtv
								, _palette[index]
								, _num
								, _rect
								);
					}
				}
			}
			else
			{
				float frgba[4] =
				{
					_clear.m_index[0]*1.0f/255.0f,
					_clear.m_index[1]*1.0f/255.0f,
					_clear.m_index[2]*1.0f/255.0f,
					_clear.m_index[3]*1.0f/255.0f,
				};
				for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
				{
					D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtvDescriptor.ptr + ii * rtvDescriptorSize };
					_commandList->ClearRenderTargetView(rtv
						, frgba
						, _num
						, _rect
						);
				}
			}
		}

		if (isValid(m_depth)
		&& (BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL) & _clear.m_flags)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE dsvDescriptor = s_renderD3D12->m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
			uint32_t dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
			dsvDescriptor.ptr += (1 + fbhIdx) * dsvDescriptorSize;

			DWORD flags = 0;
			flags |= (_clear.m_flags & BGFX_CLEAR_DEPTH)   ? D3D12_CLEAR_FLAG_DEPTH   : 0;
			flags |= (_clear.m_flags & BGFX_CLEAR_STENCIL) ? D3D12_CLEAR_FLAG_STENCIL : 0;

			_commandList->ClearDepthStencilView(dsvDescriptor
				, D3D12_CLEAR_FLAGS(flags)
				, _clear.m_depth
				, _clear.m_stencil
				, _num
				, _rect
				);
		}
	}

	void RendererContextD3D12::submit(Frame* _render, ClearQuad& /*_clearQuad*/, TextVideoMemBlitter& _textVideoMemBlitter)
	{
//		PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), L"rendererSubmit");

		updateResolution(_render->m_resolution);

		int64_t elapsed = -bx::getHPCounter();
		int64_t captureElapsed = 0;

		if (0 < _render->m_iboffset)
		{
			TransientIndexBuffer* ib = _render->m_transientIb;
			m_indexBuffers[ib->handle.idx].update(m_commandList, 0, _render->m_iboffset, ib->data);
		}

		if (0 < _render->m_vboffset)
		{
			TransientVertexBuffer* vb = _render->m_transientVb;
			m_vertexBuffers[vb->handle.idx].update(m_commandList, 0, _render->m_vboffset, vb->data);
		}

		_render->sort();

		RenderDraw currentState;
		currentState.clear();
		currentState.m_flags = BGFX_STATE_NONE;
		currentState.m_stencil = packStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);

		_render->m_hmdInitialized = false;

		const bool hmdEnabled = false;
		ViewState viewState(_render, hmdEnabled);
		viewState.reset(_render, hmdEnabled);

// 		bool wireframe = !!(_render->m_debug&BGFX_DEBUG_WIREFRAME);
// 		setDebugWireframe(wireframe);

		uint16_t currentSamplerStateIdx = invalidHandle;
		uint16_t currentProgramIdx      = invalidHandle;
		uint32_t currentBindHash = 0;
		ID3D12PipelineState* currentPso = NULL;
		SortKey key;
		uint16_t view = UINT16_MAX;
		FrameBufferHandle fbh = BGFX_INVALID_HANDLE;
		uint32_t blendFactor = 0;

		const uint64_t primType = _render->m_debug&BGFX_DEBUG_WIREFRAME ? BGFX_STATE_PT_LINES : 0;
		uint8_t primIndex = uint8_t(primType >> BGFX_STATE_PT_SHIFT);
		PrimInfo prim = s_primInfo[primIndex];

		bool wasCompute = false;
		bool viewHasScissor = false;
		bool restoreScissor = false;
		Rect viewScissorRect;
		viewScissorRect.clear();

		uint32_t statsNumPrimsSubmitted[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumPrimsRendered[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumInstances[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumIndices = 0;
		uint32_t statsKeyType[2] = {};

		m_backBufferColorIdx = m_swapChain->GetCurrentBackBufferIndex();

		const uint64_t f0 = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_FACTOR);
		const uint64_t f1 = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_INV_FACTOR, BGFX_STATE_BLEND_INV_FACTOR);

		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
		ScratchBufferD3D12& scratchBuffer = m_scratchBuffer[m_backBufferColorIdx];
		scratchBuffer.reset(gpuHandle);

		D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = UINT64_C(0);
		StateCacheLru<D3D12_GPU_DESCRIPTOR_HANDLE, 64> bindLru;

		setResourceBarrier(m_commandList
			, m_backBufferColor[m_backBufferColorIdx]
			, D3D12_RESOURCE_STATE_PRESENT
			, D3D12_RESOURCE_STATE_RENDER_TARGET
			);

		if (0 == (_render->m_debug&BGFX_DEBUG_IFH) )
		{
			m_batch.begin();

// 			uint8_t eye = 0;
// 			uint8_t restartState = 0;
			viewState.m_rect = _render->m_rect[0];

			int32_t numItems = _render->m_num;
			for (int32_t item = 0, restartItem = numItems; item < numItems || restartItem < numItems;)
			{
				const bool isCompute = key.decode(_render->m_sortKeys[item], _render->m_viewRemap);
				statsKeyType[isCompute]++;

				const bool viewChanged = 0
					|| key.m_view != view
					|| item == numItems
					;

				const RenderItem& renderItem = _render->m_renderItem[_render->m_sortValues[item] ];
				++item;

				if (viewChanged)
				{
					m_batch.flush(m_commandList, true);
					kick();

					view = key.m_view;
					currentPso = NULL;
					currentSamplerStateIdx = invalidHandle;
					currentProgramIdx      = invalidHandle;

					fbh = _render->m_fb[view];
					setFrameBuffer(fbh);

					viewState.m_rect = _render->m_rect[view];
					const Rect& rect        = _render->m_rect[view];
					const Rect& scissorRect = _render->m_scissor[view];
					viewHasScissor  = !scissorRect.isZero();
					viewScissorRect = viewHasScissor ? scissorRect : rect;

					D3D12_VIEWPORT vp;
					vp.TopLeftX = rect.m_x;
					vp.TopLeftY = rect.m_y;
					vp.Width    = rect.m_width;
					vp.Height   = rect.m_height;
					vp.MinDepth = 0.0f;
					vp.MaxDepth = 1.0f;
					m_commandList->RSSetViewports(1, &vp);

					D3D12_RECT rc;
					rc.left   = viewScissorRect.m_x;
					rc.top    = viewScissorRect.m_y;
					rc.right  = viewScissorRect.m_x + viewScissorRect.m_width;
					rc.bottom = viewScissorRect.m_y + viewScissorRect.m_height;
					m_commandList->RSSetScissorRects(1, &rc);
					restoreScissor = false;

					Clear& clr = _render->m_clear[view];
					if (BGFX_CLEAR_NONE != clr.m_flags)
					{
						Rect clearRect = rect;
						clearRect.intersect(rect, viewScissorRect);
						clearQuad(clearRect, clr, _render->m_clearColor);
					}

					prim = s_primInfo[BX_COUNTOF(s_primName)]; // Force primitive type update.
				}

				if (isCompute)
				{
					if (!wasCompute)
					{
						wasCompute = true;

						m_commandList->SetComputeRootSignature(m_rootSignature);
						ID3D12DescriptorHeap* heaps[] = {
							m_samplerAllocator.getHeap(),
							scratchBuffer.getHeap(),
						};
						m_commandList->SetDescriptorHeaps(BX_COUNTOF(heaps), heaps);
					}

					const RenderCompute& compute = renderItem.compute;

					ID3D12PipelineState* pso = getPipelineState(key.m_program);
					if (pso != currentPso)
					{
						currentPso = pso;
						m_commandList->SetPipelineState(pso);
						currentBindHash = 0;
					}

					uint32_t bindHash = bx::hashMurmur2A(compute.m_bind, sizeof(compute.m_bind) );
					if (currentBindHash != bindHash)
					{
						currentBindHash  = bindHash;

						D3D12_GPU_DESCRIPTOR_HANDLE* srv = bindLru.find(bindHash);
						if (NULL == srv)
						{
							D3D12_GPU_DESCRIPTOR_HANDLE srvHandle[BGFX_MAX_COMPUTE_BINDINGS] = {};
							uint32_t samplerFlags[BGFX_MAX_COMPUTE_BINDINGS] = {};

							for (uint32_t ii = 0; ii < BGFX_MAX_COMPUTE_BINDINGS; ++ii)
							{
								const Binding& bind = compute.m_bind[ii];
								if (invalidHandle != bind.m_idx)
								{
									switch (bind.m_type)
									{
									case Binding::Image:
										{
											TextureD3D12& texture = m_textures[bind.m_idx];

											if (Access::Read != bind.m_un.m_compute.m_access)
											{
												texture.setState(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
												scratchBuffer.allocUav(srvHandle[ii], texture, bind.m_un.m_compute.m_mip);
											}
											else
											{
												texture.setState(m_commandList, D3D12_RESOURCE_STATE_GENERIC_READ);
												scratchBuffer.allocSrv(srvHandle[ii], texture, bind.m_un.m_compute.m_mip);
												samplerFlags[ii] = texture.m_flags;
											}
										}
										break;

									case Binding::IndexBuffer:
									case Binding::VertexBuffer:
										{
											BufferD3D12& buffer = Binding::IndexBuffer == bind.m_type
												? m_indexBuffers[bind.m_idx]
												: m_vertexBuffers[bind.m_idx]
												;

											if (Access::Read != bind.m_un.m_compute.m_access)
											{
												buffer.setState(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
												scratchBuffer.allocUav(srvHandle[ii], buffer);
											}
											else
											{
												buffer.setState(m_commandList, D3D12_RESOURCE_STATE_GENERIC_READ);
												scratchBuffer.allocSrv(srvHandle[ii], buffer);
											}
										}
										break;
									}
								}
							}

							uint16_t samplerStateIdx = getSamplerState(samplerFlags, BGFX_MAX_COMPUTE_BINDINGS);
							if (samplerStateIdx != currentSamplerStateIdx)
							{
								currentSamplerStateIdx = samplerStateIdx;
								m_commandList->SetComputeRootDescriptorTable(Rdt::Sampler, m_samplerAllocator.get(samplerStateIdx) );
							}

							m_commandList->SetComputeRootDescriptorTable(Rdt::SRV, srvHandle[0]);
							m_commandList->SetComputeRootDescriptorTable(Rdt::UAV, srvHandle[0]);
						}
						else
						{
							m_commandList->SetComputeRootDescriptorTable(Rdt::SRV, *srv);
							m_commandList->SetComputeRootDescriptorTable(Rdt::UAV, *srv);
						}
					}

					if (compute.m_constBegin < compute.m_constEnd
					||  currentProgramIdx != key.m_program)
					{
						rendererUpdateUniforms(this, _render->m_constantBuffer, compute.m_constBegin, compute.m_constEnd);

						currentProgramIdx = key.m_program;
						ProgramD3D12& program = m_program[key.m_program];

						ConstantBuffer* vcb = program.m_vsh->m_constantBuffer;
						if (NULL != vcb)
						{
							commit(*vcb);
						}

						viewState.setPredefined<4>(this, view, 0, program, _render, compute);
						commitShaderConstants(key.m_program, gpuAddress);
						m_commandList->SetComputeRootConstantBufferView(Rdt::CBV, gpuAddress);
					}

					if (isValid(compute.m_indirectBuffer) )
					{
						const VertexBufferD3D12& vb = m_vertexBuffers[compute.m_indirectBuffer.idx];

						uint32_t numDrawIndirect = UINT16_MAX == compute.m_numIndirect
							? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
							: compute.m_numIndirect
							;

						uint32_t args = compute.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
						for (uint32_t ii = 0; ii < numDrawIndirect; ++ii)
						{
//							m_commandList->ExecuteIndirect(ptr, args);
							args += BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
						}
					}
					else
					{
						m_commandList->Dispatch(compute.m_numX, compute.m_numY, compute.m_numZ);
					}

					continue;
				}

				const RenderDraw& draw = renderItem.draw;

				const uint64_t newFlags = draw.m_flags;
				uint64_t changedFlags = currentState.m_flags ^ draw.m_flags;
				currentState.m_flags = newFlags;

				const uint64_t newStencil = draw.m_stencil;
				uint64_t changedStencil = (currentState.m_stencil ^ draw.m_stencil) & BGFX_STENCIL_FUNC_REF_MASK;
				currentState.m_stencil = newStencil;

				if (viewChanged
				||  wasCompute)
				{
					if (wasCompute)
					{
						wasCompute = false;
						kick();
					}

					if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
					{
						BX_UNUSED(s_viewNameW);
// 						wchar_t* viewNameW = s_viewNameW[view];
// 						viewNameW[3] = L' ';
// 						PIX_ENDEVENT();
// 						PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), viewNameW);
					}

					currentSamplerStateIdx = invalidHandle;
					currentProgramIdx      = invalidHandle;

					m_commandList->SetGraphicsRootSignature(m_rootSignature);
					ID3D12DescriptorHeap* heaps[] = {
						m_samplerAllocator.getHeap(),
						scratchBuffer.getHeap(),
					};
					m_commandList->SetDescriptorHeaps(BX_COUNTOF(heaps), heaps);

					currentState.clear();
					currentState.m_scissor = !draw.m_scissor;
					changedFlags = BGFX_STATE_MASK;
					changedStencil = packStencil(BGFX_STENCIL_MASK, BGFX_STENCIL_MASK);
					currentState.m_flags = newFlags;
					currentState.m_stencil = newStencil;

					const uint64_t pt = newFlags&BGFX_STATE_PT_MASK;
					primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
				}

				if (isValid(draw.m_vertexBuffer) )
				{
					const uint64_t state = draw.m_flags;
					bool hasFactor = 0
						|| f0 == (state & f0)
						|| f1 == (state & f1)
						;

					const VertexBufferD3D12& vb = m_vertexBuffers[draw.m_vertexBuffer.idx];
					uint16_t declIdx = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;

					ID3D12PipelineState* pso =
						getPipelineState(state
							, draw.m_stencil
							, declIdx
							, key.m_program
							, uint8_t(draw.m_instanceDataStride/16)
							);

					uint16_t scissor = draw.m_scissor;
					uint32_t bindHash = bx::hashMurmur2A(draw.m_bind, sizeof(draw.m_bind) );
					if (currentBindHash != bindHash
					||  0 != changedStencil
					|| (hasFactor && blendFactor != draw.m_rgba)
					|| (0 != (BGFX_STATE_PT_MASK & changedFlags)
					||  prim.m_toplogy != s_primInfo[primIndex].m_toplogy)
					||  currentState.m_scissor != scissor
					||  pso != currentPso)
					{
						m_batch.flush(m_commandList);
					}

					if (currentBindHash != bindHash)
					{
						currentBindHash  = bindHash;

						D3D12_GPU_DESCRIPTOR_HANDLE* srv = bindLru.find(bindHash);
						if (NULL == srv)
						{
							D3D12_GPU_DESCRIPTOR_HANDLE srvHandle[BGFX_CONFIG_MAX_TEXTURE_SAMPLERS];
							uint32_t samplerFlags[BGFX_CONFIG_MAX_TEXTURE_SAMPLERS];
							{
								srvHandle[0].ptr = 0;
								for (uint32_t stage = 0; stage < BGFX_CONFIG_MAX_TEXTURE_SAMPLERS; ++stage)
								{
									const Binding& bind = draw.m_bind[stage];
									if (invalidHandle != bind.m_idx)
									{
										TextureD3D12& texture = m_textures[bind.m_idx];
										texture.setState(m_commandList, D3D12_RESOURCE_STATE_GENERIC_READ);
										scratchBuffer.allocSrv(srvHandle[stage], texture);
										samplerFlags[stage] = (0 == (BGFX_SAMPLER_DEFAULT_FLAGS & bind.m_un.m_draw.m_flags)
											? bind.m_un.m_draw.m_flags
											: texture.m_flags
											) & BGFX_TEXTURE_SAMPLER_BITS_MASK
											;
									}
									else
									{
										memcpy(&srvHandle[stage], &srvHandle[0], sizeof(D3D12_GPU_DESCRIPTOR_HANDLE) );
										samplerFlags[stage] = 0;
									}
								}
							}

							if (srvHandle[0].ptr != 0)
							{
								uint16_t samplerStateIdx = getSamplerState(samplerFlags);
								if (samplerStateIdx != currentSamplerStateIdx)
								{
									currentSamplerStateIdx = samplerStateIdx;
									m_commandList->SetGraphicsRootDescriptorTable(Rdt::Sampler, m_samplerAllocator.get(samplerStateIdx) );
								}

								m_commandList->SetGraphicsRootDescriptorTable(Rdt::SRV, srvHandle[0]);

								bindLru.add(bindHash, srvHandle[0], 0);
							}
						}
						else
						{
							m_commandList->SetGraphicsRootDescriptorTable(Rdt::SRV, *srv);
						}
					}

					if (0 != changedStencil)
					{
						const uint32_t fstencil = unpackStencil(0, draw.m_stencil);
						const uint32_t ref = (fstencil&BGFX_STENCIL_FUNC_REF_MASK)>>BGFX_STENCIL_FUNC_REF_SHIFT;
						m_commandList->OMSetStencilRef(ref);
					}

					if (hasFactor
					&&  blendFactor != draw.m_rgba)
					{
						blendFactor = draw.m_rgba;

						float bf[4];
						bf[0] = ( (draw.m_rgba>>24)     )/255.0f;
						bf[1] = ( (draw.m_rgba>>16)&0xff)/255.0f;
						bf[2] = ( (draw.m_rgba>> 8)&0xff)/255.0f;
						bf[3] = ( (draw.m_rgba    )&0xff)/255.0f;
						m_commandList->OMSetBlendFactor(bf);
					}

					if (0 != (BGFX_STATE_PT_MASK & changedFlags)
					||  prim.m_toplogy != s_primInfo[primIndex].m_toplogy)
					{
						const uint64_t pt = newFlags&BGFX_STATE_PT_MASK;
						primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
						prim = s_primInfo[primIndex];
						m_commandList->IASetPrimitiveTopology(prim.m_toplogy);
					}

					if (currentState.m_scissor != scissor)
					{
						currentState.m_scissor = scissor;

						if (UINT16_MAX == scissor)
						{
							if (restoreScissor
							||  viewHasScissor)
							{
								restoreScissor = false;
								D3D12_RECT rc;
								rc.left   = viewScissorRect.m_x;
								rc.top    = viewScissorRect.m_y;
								rc.right  = viewScissorRect.m_x + viewScissorRect.m_width;
								rc.bottom = viewScissorRect.m_y + viewScissorRect.m_height;
								m_commandList->RSSetScissorRects(1, &rc);
							}
						}
						else
						{
							restoreScissor = true;
							Rect scissorRect;
							scissorRect.intersect(viewScissorRect,_render->m_rectCache.m_cache[scissor]);
							D3D12_RECT rc;
							rc.left   = scissorRect.m_x;
							rc.top    = scissorRect.m_y;
							rc.right  = scissorRect.m_x + scissorRect.m_width;
							rc.bottom = scissorRect.m_y + scissorRect.m_height;
							m_commandList->RSSetScissorRects(1, &rc);
						}
					}

					if (pso != currentPso)
					{
						currentPso = pso;
						m_commandList->SetPipelineState(pso);
					}

					if (draw.m_constBegin < draw.m_constEnd
					||  currentProgramIdx != key.m_program
					||  BGFX_STATE_ALPHA_REF_MASK & changedFlags)
					{
						rendererUpdateUniforms(this, _render->m_constantBuffer, draw.m_constBegin, draw.m_constEnd);

						currentProgramIdx = key.m_program;
						ProgramD3D12& program = m_program[key.m_program];

						ConstantBuffer* vcb = program.m_vsh->m_constantBuffer;
						if (NULL != vcb)
						{
							commit(*vcb);
						}

						ConstantBuffer* fcb = program.m_fsh->m_constantBuffer;
						if (NULL != fcb)
						{
							commit(*fcb);
						}

						uint32_t ref = (newFlags&BGFX_STATE_ALPHA_REF_MASK)>>BGFX_STATE_ALPHA_REF_SHIFT;
						viewState.m_alphaRef = ref/255.0f;
						viewState.setPredefined<4>(this, view, 0, program, _render, draw);
						commitShaderConstants(key.m_program, gpuAddress);
					}

					uint32_t numIndices        = m_batch.draw(m_commandList, gpuAddress, draw);
					uint32_t numPrimsSubmitted = numIndices / prim.m_div - prim.m_sub;
					uint32_t numPrimsRendered  = numPrimsSubmitted*draw.m_numInstances;

					statsNumPrimsSubmitted[primIndex] += numPrimsSubmitted;
					statsNumPrimsRendered[primIndex]  += numPrimsRendered;
					statsNumInstances[primIndex]      += draw.m_numInstances;
					statsNumIndices                   += numIndices;
				}
			}

			m_batch.end(m_commandList);
		}

		int64_t now = bx::getHPCounter();
		elapsed += now;

		static int64_t last = now;
		int64_t frameTime = now - last;
		last = now;

		static int64_t min = frameTime;
		static int64_t max = frameTime;
		min = bx::int64_min(min, frameTime);
		max = bx::int64_max(max, frameTime);

		static int64_t presentMin = m_presentElapsed;
		static int64_t presentMax = m_presentElapsed;
		presentMin = bx::int64_min(presentMin, m_presentElapsed);
		presentMax = bx::int64_max(presentMax, m_presentElapsed);

		if (_render->m_debug & (BGFX_DEBUG_IFH | BGFX_DEBUG_STATS) )
		{
//			PIX_BEGINEVENT(D3DCOLOR_RGBA(0x40, 0x40, 0x40, 0xff), L"debugstats");

			TextVideoMem& tvm = m_textVideoMem;

			static int64_t next = now;

			if (now >= next)
			{
				next = now + bx::getHPFrequency();
				double freq = double(bx::getHPFrequency() );
				double toMs = 1000.0 / freq;

				tvm.clear();
				uint16_t pos = 0;
				tvm.printf(0, pos++, BGFX_CONFIG_DEBUG ? 0x89 : 0x8f
					, " %s / " BX_COMPILER_NAME " / " BX_CPU_NAME " / " BX_ARCH_NAME " / " BX_PLATFORM_NAME " "
					, getRendererName()
					);

				const DXGI_ADAPTER_DESC& desc = m_adapterDesc;
				char description[BX_COUNTOF(desc.Description)];
				wcstombs(description, desc.Description, BX_COUNTOF(desc.Description) );
				tvm.printf(0, pos++, 0x8f, " Device: %s", description);

				char dedicatedVideo[16];
				bx::prettify(dedicatedVideo, BX_COUNTOF(dedicatedVideo), desc.DedicatedVideoMemory);

				char dedicatedSystem[16];
				bx::prettify(dedicatedSystem, BX_COUNTOF(dedicatedSystem), desc.DedicatedSystemMemory);

				char sharedSystem[16];
				bx::prettify(sharedSystem, BX_COUNTOF(sharedSystem), desc.SharedSystemMemory);

				tvm.printf(0, pos++, 0x8f, " Memory: %s (video), %s (system), %s (shared)"
					, dedicatedVideo
					, dedicatedSystem
					, sharedSystem
					);

				DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
				DX_CHECK(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo) );

				char budget[16];
				bx::prettify(budget, BX_COUNTOF(budget), memInfo.Budget);

				char currentUsage[16];
				bx::prettify(currentUsage, BX_COUNTOF(currentUsage), memInfo.CurrentUsage);

				char availableForReservation[16];
				bx::prettify(availableForReservation, BX_COUNTOF(currentUsage), memInfo.AvailableForReservation);

				char currentReservation[16];
				bx::prettify(currentReservation, BX_COUNTOF(currentReservation), memInfo.CurrentReservation);

				tvm.printf(0, pos++, 0x8f, " Budget: %s, Usage: %s, AvailRes: %s, CurrRes: %s "
					, budget
					, currentUsage
					, availableForReservation
					, currentReservation
					);

				pos = 10;
				tvm.printf(10, pos++, 0x8e, "       Frame: % 7.3f, % 7.3f \x1f, % 7.3f \x1e [ms] / % 6.2f FPS "
					, double(frameTime)*toMs
					, double(min)*toMs
					, double(max)*toMs
					, freq/frameTime
					);
				tvm.printf(10, pos++, 0x8e, "     Present: % 7.3f, % 7.3f \x1f, % 7.3f \x1e [ms] "
					, double(m_presentElapsed)*toMs
					, double(presentMin)*toMs
					, double(presentMax)*toMs
					);

				char hmd[16];
				bx::snprintf(hmd, BX_COUNTOF(hmd), ", [%c] HMD ", hmdEnabled ? '\xfe' : ' ');

				const uint32_t msaa = (m_resolution.m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT;
				tvm.printf(10, pos++, 0x8e, " Reset flags: [%c] vsync, [%c] MSAAx%d%s, [%c] MaxAnisotropy "
					, !!(m_resolution.m_flags&BGFX_RESET_VSYNC) ? '\xfe' : ' '
					, 0 != msaa ? '\xfe' : ' '
					, 1<<msaa
					, ", no-HMD "
					, !!(m_resolution.m_flags&BGFX_RESET_MAXANISOTROPY) ? '\xfe' : ' '
					);

				double elapsedCpuMs = double(elapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "   Submitted: %5d (draw %5d, compute %4d) / CPU %7.4f [ms] "
					, _render->m_num
					, statsKeyType[0]
					, statsKeyType[1]
					, elapsedCpuMs
					);

				for (uint32_t ii = 0; ii < BX_COUNTOF(s_primName); ++ii)
				{
					tvm.printf(10, pos++, 0x8e, "   %9s: %7d (#inst: %5d), submitted: %7d "
						, s_primName[ii]
						, statsNumPrimsRendered[ii]
						, statsNumInstances[ii]
						, statsNumPrimsSubmitted[ii]
						);
				}

				tvm.printf(10, pos++, 0x8e, "       Batch: %7dx%d indirect, %7d immediate "
					, m_batch.m_stats.m_numIndirect[BatchD3D12::Draw]
					, m_batch.m_maxDrawPerBatch
					, m_batch.m_stats.m_numImmediate[BatchD3D12::Draw]
					);

				tvm.printf(10, pos++, 0x8e, "              %7dx%d indirect, %7d immediate "
					, m_batch.m_stats.m_numIndirect[BatchD3D12::DrawIndexed]
					, m_batch.m_maxDrawPerBatch
					, m_batch.m_stats.m_numImmediate[BatchD3D12::DrawIndexed]
					);

// 				if (NULL != m_renderdocdll)
// 				{
// 					tvm.printf(tvm.m_width-27, 0, 0x1f, " [F11 - RenderDoc capture] ");
// 				}

				tvm.printf(10, pos++, 0x8e, "      Indices: %7d ", statsNumIndices);
				tvm.printf(10, pos++, 0x8e, " Uniform size: %7d ", _render->m_constEnd);
				tvm.printf(10, pos++, 0x8e, "     DVB size: %7d ", _render->m_vboffset);
				tvm.printf(10, pos++, 0x8e, "     DIB size: %7d ", _render->m_iboffset);

				pos++;
				tvm.printf(10, pos++, 0x8e, " State cache:                        ");
				tvm.printf(10, pos++, 0x8e, " PSO    | Sampler | Bind   | Queued  ");
				tvm.printf(10, pos++, 0x8e, " %6d |  %6d | %6d | %6d  "
					, m_pipelineStateCache.getCount()
					, m_samplerStateCache.getCount()
					, bindLru.getCount()
					, m_cmd.m_control.available()
					);
				pos++;

				double captureMs = double(captureElapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "     Capture: %7.4f [ms] ", captureMs);

				uint8_t attr[2] = { 0x89, 0x8a };
				uint8_t attrIndex = _render->m_waitSubmit < _render->m_waitRender;

				tvm.printf(10, pos++, attr[attrIndex&1], " Submit wait: %7.4f [ms] ", _render->m_waitSubmit*toMs);
				tvm.printf(10, pos++, attr[(attrIndex+1)&1], " Render wait: %7.4f [ms] ", _render->m_waitRender*toMs);

				min = frameTime;
				max = frameTime;
				presentMin = m_presentElapsed;
				presentMax = m_presentElapsed;
			}

			blit(this, _textVideoMemBlitter, tvm);

//			PIX_ENDEVENT();
		}
		else if (_render->m_debug & BGFX_DEBUG_TEXT)
		{
//			PIX_BEGINEVENT(D3DCOLOR_RGBA(0x40, 0x40, 0x40, 0xff), L"debugtext");

			blit(this, _textVideoMemBlitter, _render->m_textVideoMem);

//			PIX_ENDEVENT();
		}

		setResourceBarrier(m_commandList
			, m_backBufferColor[m_backBufferColorIdx]
			, D3D12_RESOURCE_STATE_RENDER_TARGET
			, D3D12_RESOURCE_STATE_PRESENT
			);
		m_backBufferColorFence[m_backBufferColorIdx] = kick();
	}

} /* namespace d3d12 */ } // namespace bgfx

#else

namespace bgfx { namespace d3d12
{
	RendererContextI* rendererCreate()
	{
		return NULL;
	}

	void rendererDestroy()
	{
	}
} /* namespace d3d12 */ } // namespace bgfx

#endif // BGFX_CONFIG_RENDERER_DIRECT3D12
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_DIRECT3D9
#	include "renderer_d3d9.h"

namespace bgfx { namespace d3d9
{
	static wchar_t s_viewNameW[BGFX_CONFIG_MAX_VIEWS][BGFX_CONFIG_MAX_VIEW_NAME];

	struct PrimInfo
	{
		D3DPRIMITIVETYPE m_type;
		uint32_t m_min;
		uint32_t m_div;
		uint32_t m_sub;
	};

	static const PrimInfo s_primInfo[] =
	{
		{ D3DPT_TRIANGLELIST,  3, 3, 0 },
		{ D3DPT_TRIANGLESTRIP, 3, 1, 2 },
		{ D3DPT_LINELIST,      2, 2, 0 },
		{ D3DPT_LINESTRIP,     2, 1, 1 },
		{ D3DPT_POINTLIST,     1, 1, 0 },
		{ D3DPRIMITIVETYPE(0), 0, 0, 0 },
	};

	static const char* s_primName[] =
	{
		"TriList",
		"TriStrip",
		"Line",
		"LineStrip",
		"Point",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_primInfo) == BX_COUNTOF(s_primName)+1);

	static const D3DMULTISAMPLE_TYPE s_checkMsaa[] =
	{
		D3DMULTISAMPLE_NONE,
		D3DMULTISAMPLE_2_SAMPLES,
		D3DMULTISAMPLE_4_SAMPLES,
		D3DMULTISAMPLE_8_SAMPLES,
		D3DMULTISAMPLE_16_SAMPLES,
	};

	static Msaa s_msaa[] =
	{
		{ D3DMULTISAMPLE_NONE,       0 },
		{ D3DMULTISAMPLE_2_SAMPLES,  0 },
		{ D3DMULTISAMPLE_4_SAMPLES,  0 },
		{ D3DMULTISAMPLE_8_SAMPLES,  0 },
		{ D3DMULTISAMPLE_16_SAMPLES, 0 },
	};

	struct Blend
	{
		D3DBLEND m_src;
		D3DBLEND m_dst;
		bool m_factor;
	};

	static const Blend s_blendFactor[] =
	{
		{ (D3DBLEND)0,             (D3DBLEND)0,             false }, // ignored
		{ D3DBLEND_ZERO,           D3DBLEND_ZERO,           false }, // ZERO
		{ D3DBLEND_ONE,            D3DBLEND_ONE,            false }, // ONE
		{ D3DBLEND_SRCCOLOR,       D3DBLEND_SRCCOLOR,       false }, // SRC_COLOR
		{ D3DBLEND_INVSRCCOLOR,    D3DBLEND_INVSRCCOLOR,    false }, // INV_SRC_COLOR
		{ D3DBLEND_SRCALPHA,       D3DBLEND_SRCALPHA,       false }, // SRC_ALPHA
		{ D3DBLEND_INVSRCALPHA,    D3DBLEND_INVSRCALPHA,    false }, // INV_SRC_ALPHA
		{ D3DBLEND_DESTALPHA,      D3DBLEND_DESTALPHA,      false }, // DST_ALPHA
		{ D3DBLEND_INVDESTALPHA,   D3DBLEND_INVDESTALPHA,   false }, // INV_DST_ALPHA
		{ D3DBLEND_DESTCOLOR,      D3DBLEND_DESTCOLOR,      false }, // DST_COLOR
		{ D3DBLEND_INVDESTCOLOR,   D3DBLEND_INVDESTCOLOR,   false }, // INV_DST_COLOR
		{ D3DBLEND_SRCALPHASAT,    D3DBLEND_ONE,            false }, // SRC_ALPHA_SAT
		{ D3DBLEND_BLENDFACTOR,    D3DBLEND_BLENDFACTOR,    true  }, // FACTOR
		{ D3DBLEND_INVBLENDFACTOR, D3DBLEND_INVBLENDFACTOR, true  }, // INV_FACTOR
	};

	static const D3DBLENDOP s_blendEquation[] =
	{
		D3DBLENDOP_ADD,
		D3DBLENDOP_SUBTRACT,
		D3DBLENDOP_REVSUBTRACT,
		D3DBLENDOP_MIN,
		D3DBLENDOP_MAX,
	};

	static const D3DCMPFUNC s_cmpFunc[] =
	{
		(D3DCMPFUNC)0, // ignored
		D3DCMP_LESS,
		D3DCMP_LESSEQUAL,
		D3DCMP_EQUAL,
		D3DCMP_GREATEREQUAL,
		D3DCMP_GREATER,
		D3DCMP_NOTEQUAL,
		D3DCMP_NEVER,
		D3DCMP_ALWAYS,
	};

	static const D3DSTENCILOP s_stencilOp[] =
	{
		D3DSTENCILOP_ZERO,
		D3DSTENCILOP_KEEP,
		D3DSTENCILOP_REPLACE,
		D3DSTENCILOP_INCR,
		D3DSTENCILOP_INCRSAT,
		D3DSTENCILOP_DECR,
		D3DSTENCILOP_DECRSAT,
		D3DSTENCILOP_INVERT,
	};

	static const D3DRENDERSTATETYPE s_stencilFuncRs[] =
	{
		D3DRS_STENCILFUNC,
		D3DRS_CCW_STENCILFUNC,
	};

	static const D3DRENDERSTATETYPE s_stencilFailRs[] =
	{
		D3DRS_STENCILFAIL,
		D3DRS_CCW_STENCILFAIL,
	};

	static const D3DRENDERSTATETYPE s_stencilZFailRs[] =
	{
		D3DRS_STENCILZFAIL,
		D3DRS_CCW_STENCILZFAIL,
	};

	static const D3DRENDERSTATETYPE s_stencilZPassRs[] =
	{
		D3DRS_STENCILPASS,
		D3DRS_CCW_STENCILPASS,
	};

	static const D3DCULL s_cullMode[] =
	{
		D3DCULL_NONE,
		D3DCULL_CW,
		D3DCULL_CCW,
	};

	static const D3DFORMAT s_checkColorFormats[] =
	{
		D3DFMT_UNKNOWN,
		D3DFMT_A8R8G8B8, D3DFMT_UNKNOWN,
		D3DFMT_R32F, D3DFMT_R16F, D3DFMT_G16R16, D3DFMT_A8R8G8B8, D3DFMT_UNKNOWN,

		D3DFMT_UNKNOWN, // terminator
	};

	static D3DFORMAT s_colorFormat[] =
	{
		D3DFMT_UNKNOWN, // ignored
		D3DFMT_A8R8G8B8,
		D3DFMT_A2B10G10R10,
		D3DFMT_A16B16G16R16,
		D3DFMT_A16B16G16R16F,
		D3DFMT_R16F,
		D3DFMT_R32F,
	};

	static const D3DTEXTUREADDRESS s_textureAddress[] =
	{
		D3DTADDRESS_WRAP,
		D3DTADDRESS_MIRROR,
		D3DTADDRESS_CLAMP,
	};

	static const D3DTEXTUREFILTERTYPE s_textureFilter[] =
	{
		D3DTEXF_LINEAR,
		D3DTEXF_POINT,
		D3DTEXF_ANISOTROPIC,
	};

	struct TextureFormatInfo
	{
		D3DFORMAT m_fmt;
	};

	static TextureFormatInfo s_textureFormat[] =
	{
		{ D3DFMT_DXT1          }, // BC1
		{ D3DFMT_DXT3          }, // BC2
		{ D3DFMT_DXT5          }, // BC3
		{ D3DFMT_UNKNOWN       }, // BC4
		{ D3DFMT_UNKNOWN       }, // BC5
		{ D3DFMT_UNKNOWN       }, // BC6H
		{ D3DFMT_UNKNOWN       }, // BC7
		{ D3DFMT_UNKNOWN       }, // ETC1
		{ D3DFMT_UNKNOWN       }, // ETC2
		{ D3DFMT_UNKNOWN       }, // ETC2A
		{ D3DFMT_UNKNOWN       }, // ETC2A1
		{ D3DFMT_UNKNOWN       }, // PTC12
		{ D3DFMT_UNKNOWN       }, // PTC14
		{ D3DFMT_UNKNOWN       }, // PTC12A
		{ D3DFMT_UNKNOWN       }, // PTC14A
		{ D3DFMT_UNKNOWN       }, // PTC22
		{ D3DFMT_UNKNOWN       }, // PTC24
		{ D3DFMT_UNKNOWN       }, // Unknown
		{ D3DFMT_A1            }, // R1
		{ D3DFMT_L8            }, // R8
		{ D3DFMT_UNKNOWN       }, // R8I
		{ D3DFMT_UNKNOWN       }, // R8U
		{ D3DFMT_UNKNOWN       }, // R8S
		{ D3DFMT_L16           }, // R16
		{ D3DFMT_UNKNOWN       }, // R16I
		{ D3DFMT_UNKNOWN       }, // R16U
		{ D3DFMT_R16F          }, // R16F
		{ D3DFMT_UNKNOWN       }, // R16S
		{ D3DFMT_UNKNOWN       }, // R32U
		{ D3DFMT_R32F          }, // R32F
		{ D3DFMT_A8L8          }, // RG8
		{ D3DFMT_UNKNOWN       }, // RG8I
		{ D3DFMT_UNKNOWN       }, // RG8U
		{ D3DFMT_UNKNOWN       }, // RG8S
		{ D3DFMT_G16R16        }, // RG16
		{ D3DFMT_UNKNOWN       }, // RG16I
		{ D3DFMT_UNKNOWN       }, // RG16U
		{ D3DFMT_G16R16F       }, // RG16F
		{ D3DFMT_UNKNOWN       }, // RG16S
		{ D3DFMT_UNKNOWN       }, // RG32U
		{ D3DFMT_G32R32F       }, // RG32F
		{ D3DFMT_A8R8G8B8      }, // BGRA8
		{ D3DFMT_UNKNOWN       }, // RGBA8
		{ D3DFMT_UNKNOWN       }, // RGBA8I
		{ D3DFMT_UNKNOWN       }, // RGBA8U
		{ D3DFMT_UNKNOWN       }, // RGBA8S
		{ D3DFMT_A16B16G16R16  }, // RGBA16
		{ D3DFMT_UNKNOWN       }, // RGBA16I
		{ D3DFMT_UNKNOWN       }, // RGBA16U
		{ D3DFMT_A16B16G16R16F }, // RGBA16F
		{ D3DFMT_UNKNOWN       }, // RGBA16S
		{ D3DFMT_UNKNOWN       }, // RGBA32U
		{ D3DFMT_A32B32G32R32F }, // RGBA32F
		{ D3DFMT_R5G6B5        }, // R5G6B5
		{ D3DFMT_A4R4G4B4      }, // RGBA4
		{ D3DFMT_A1R5G5B5      }, // RGB5A1
		{ D3DFMT_A2B10G10R10   }, // RGB10A2
		{ D3DFMT_UNKNOWN       }, // R11G11B10F
		{ D3DFMT_UNKNOWN       }, // UnknownDepth
		{ D3DFMT_D16           }, // D16
		{ D3DFMT_D24X8         }, // D24
		{ D3DFMT_D24S8         }, // D24S8
		{ D3DFMT_D32           }, // D32
		{ D3DFMT_DF16          }, // D16F
		{ D3DFMT_DF24          }, // D24F
		{ D3DFMT_D32F_LOCKABLE }, // D32F
		{ D3DFMT_S8_LOCKABLE   }, // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_textureFormat) );

	static ExtendedFormat s_extendedFormats[ExtendedFormat::Count] =
	{
		{ D3DFMT_ATI1, 0,                     D3DRTYPE_TEXTURE, false },
		{ D3DFMT_ATI2, 0,                     D3DRTYPE_TEXTURE, false },
		{ D3DFMT_DF16, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, false },
		{ D3DFMT_DF24, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, false },
		{ D3DFMT_INST, 0,                     D3DRTYPE_SURFACE, false },
		{ D3DFMT_INTZ, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, false },
		{ D3DFMT_NULL, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, false },
		{ D3DFMT_RESZ, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, false },
		{ D3DFMT_RAWZ, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, false },
	};

#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
	static const GUID IID_IDirect3D9         = { 0x81bdcbca, 0x64d4, 0x426d, { 0xae, 0x8d, 0xad, 0x1, 0x47, 0xf4, 0x27, 0x5c } };
	static const GUID IID_IDirect3DDevice9Ex = { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0xf, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };

	typedef HRESULT (WINAPI *Direct3DCreate9ExFn)(UINT SDKVersion, IDirect3D9Ex**);
	static Direct3DCreate9ExFn     Direct3DCreate9Ex;
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX
	typedef IDirect3D9* (WINAPI *Direct3DCreate9Fn)(UINT SDKVersion);
	static Direct3DCreate9Fn       Direct3DCreate9;
	static PFN_D3DPERF_SET_MARKER  D3DPERF_SetMarker;
	static PFN_D3DPERF_BEGIN_EVENT D3DPERF_BeginEvent;
	static PFN_D3DPERF_END_EVENT   D3DPERF_EndEvent;

	struct RendererContextD3D9 : public RendererContextI
	{
		RendererContextD3D9()
			: m_d3d9(NULL)
			, m_device(NULL)
			, m_flushQuery(NULL)
			, m_swapChain(NULL)
			, m_captureTexture(NULL)
			, m_captureSurface(NULL)
			, m_captureResolve(NULL)
			, m_flags(BGFX_RESET_NONE)
			, m_maxAnisotropy(1)
			, m_initialized(false)
			, m_amd(false)
			, m_nvidia(false)
			, m_instancingSupport(false)
			, m_timerQuerySupport(false)
			, m_rtMsaa(false)
		{
		}

		~RendererContextD3D9()
		{
		}

		bool init()
		{
			struct ErrorState
			{
				enum Enum
				{
					Default,
					LoadedD3D9,
					CreatedD3D9,
					CreatedDevice,
				};
			};

			ErrorState::Enum errorState = ErrorState::Default;

			m_fbh.idx = invalidHandle;
			memset(m_uniforms, 0, sizeof(m_uniforms) );
			memset(&m_resolution, 0, sizeof(m_resolution) );

			D3DFORMAT adapterFormat = D3DFMT_X8R8G8B8;

			// http://msdn.microsoft.com/en-us/library/windows/desktop/bb172588%28v=vs.85%29.aspx
			memset(&m_params, 0, sizeof(m_params) );
			m_params.BackBufferWidth = BGFX_DEFAULT_WIDTH;
			m_params.BackBufferHeight = BGFX_DEFAULT_HEIGHT;
			m_params.BackBufferFormat = adapterFormat;
			m_params.BackBufferCount = 1;
			m_params.MultiSampleType = D3DMULTISAMPLE_NONE;
			m_params.MultiSampleQuality = 0;
			m_params.EnableAutoDepthStencil = TRUE;
			m_params.AutoDepthStencilFormat = D3DFMT_D24S8;
			m_params.Flags = D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL;
#if BX_PLATFORM_WINDOWS
			m_params.FullScreen_RefreshRateInHz = 0;
			m_params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
			m_params.SwapEffect = D3DSWAPEFFECT_DISCARD;
			m_params.hDeviceWindow = NULL;
			m_params.Windowed = true;

			RECT rect;
			GetWindowRect( (HWND)g_platformData.nwh, &rect);
			m_params.BackBufferWidth = rect.right-rect.left;
			m_params.BackBufferHeight = rect.bottom-rect.top;

			m_d3d9dll = bx::dlopen("d3d9.dll");
			BX_WARN(NULL != m_d3d9dll, "Failed to load d3d9.dll.");

			if (NULL == m_d3d9dll)
			{
				goto error;
			}

			errorState = ErrorState::LoadedD3D9;

			if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
			{
				D3DPERF_SetMarker  = (PFN_D3DPERF_SET_MARKER )bx::dlsym(m_d3d9dll, "D3DPERF_SetMarker");
				D3DPERF_BeginEvent = (PFN_D3DPERF_BEGIN_EVENT)bx::dlsym(m_d3d9dll, "D3DPERF_BeginEvent");
				D3DPERF_EndEvent   = (PFN_D3DPERF_END_EVENT  )bx::dlsym(m_d3d9dll, "D3DPERF_EndEvent");

				BX_CHECK(NULL != D3DPERF_SetMarker
					  && NULL != D3DPERF_BeginEvent
					  && NULL != D3DPERF_EndEvent
					  , "Failed to initialize PIX events."
					  );
			}
#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
			m_d3d9ex = NULL;

			Direct3DCreate9Ex = (Direct3DCreate9ExFn)bx::dlsym(m_d3d9dll, "Direct3DCreate9Ex");
			if (NULL != Direct3DCreate9Ex)
			{
				Direct3DCreate9Ex(D3D_SDK_VERSION, &m_d3d9ex);
				DX_CHECK(m_d3d9ex->QueryInterface(IID_IDirect3D9, (void**)&m_d3d9) );
				m_pool = D3DPOOL_DEFAULT;
			}
			else
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX
			{
				Direct3DCreate9 = (Direct3DCreate9Fn)bx::dlsym(m_d3d9dll, "Direct3DCreate9");
				BX_WARN(NULL != Direct3DCreate9, "Function Direct3DCreate9 not found.");

				if (NULL == Direct3DCreate9)
				{
					goto error;
				}

				m_d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
				m_pool = D3DPOOL_MANAGED;
			}

			BX_WARN(NULL != m_d3d9, "Unable to create Direct3D.");

			if (NULL == m_d3d9)
			{
				goto error;
			}

			errorState = ErrorState::CreatedD3D9;

			{
				m_adapter    = D3DADAPTER_DEFAULT;
				m_deviceType = BGFX_PCI_ID_SOFTWARE_RASTERIZER == g_caps.vendorId
					? D3DDEVTYPE_REF
					: D3DDEVTYPE_HAL
					;

				uint8_t numGPUs = uint8_t(bx::uint32_min(BX_COUNTOF(g_caps.gpu), m_d3d9->GetAdapterCount() ) );
				for (uint32_t ii = 0; ii < numGPUs; ++ii)
				{
					D3DADAPTER_IDENTIFIER9 desc;
					HRESULT hr = m_d3d9->GetAdapterIdentifier(ii, 0, &desc);
					if (SUCCEEDED(hr) )
					{
						BX_TRACE("Adapter #%d", ii);
						BX_TRACE("\tDriver: %s", desc.Driver);
						BX_TRACE("\tDescription: %s", desc.Description);
						BX_TRACE("\tDeviceName: %s", desc.DeviceName);
						BX_TRACE("\tVendorId: 0x%08x, DeviceId: 0x%08x, SubSysId: 0x%08x, Revision: 0x%08x"
							, desc.VendorId
							, desc.DeviceId
							, desc.SubSysId
							, desc.Revision
							);

						g_caps.gpu[ii].vendorId = (uint16_t)desc.VendorId;
						g_caps.gpu[ii].deviceId = (uint16_t)desc.DeviceId;

						if (D3DADAPTER_DEFAULT == m_adapter)
						{
							if ( (BGFX_PCI_ID_NONE != g_caps.vendorId ||             0 != g_caps.deviceId)
							&&   (BGFX_PCI_ID_NONE == g_caps.vendorId || desc.VendorId == g_caps.vendorId)
							&&   (               0 == g_caps.deviceId || desc.DeviceId == g_caps.deviceId) )
							{
								m_adapter = ii;
							}

							if (BX_ENABLED(BGFX_CONFIG_DEBUG_PERFHUD)
							&&  0 != strstr(desc.Description, "PerfHUD") )
							{
								m_adapter = ii;
								m_deviceType = D3DDEVTYPE_REF;
							}
						}
					}
				}

				DX_CHECK(m_d3d9->GetAdapterIdentifier(m_adapter, 0, &m_identifier) );
				m_amd    = m_identifier.VendorId == BGFX_PCI_ID_AMD;
				m_nvidia = m_identifier.VendorId == BGFX_PCI_ID_NVIDIA;
				g_caps.vendorId = 0 == m_identifier.VendorId
					? BGFX_PCI_ID_SOFTWARE_RASTERIZER
					: (uint16_t)m_identifier.VendorId
					;
				g_caps.deviceId = (uint16_t)m_identifier.DeviceId;

				uint32_t behaviorFlags[] =
				{
					D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE | D3DCREATE_PUREDEVICE,
					D3DCREATE_MIXED_VERTEXPROCESSING    | D3DCREATE_FPU_PRESERVE,
					D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
				};

				for (uint32_t ii = 0; ii < BX_COUNTOF(behaviorFlags) && NULL == m_device; ++ii)
				{
#if 0 // BGFX_CONFIG_RENDERER_DIRECT3D9EX
					DX_CHECK(m_d3d9->CreateDeviceEx(m_adapter
						, m_deviceType
						, g_platformHooks.nwh
						, behaviorFlags[ii]
						, &m_params
						, NULL
						, &m_device
						) );
#else
					DX_CHECK(m_d3d9->CreateDevice(m_adapter
						, m_deviceType
						, (HWND)g_platformData.nwh
						, behaviorFlags[ii]
						, &m_params
						, &m_device
						));
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX
				}
			}

			BX_WARN(NULL != m_device, "Unable to create Direct3D9 device.");

			if (NULL == m_device)
			{
				goto error;
			}

			errorState = ErrorState::CreatedDevice;

			m_numWindows = 1;

#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
			if (NULL != m_d3d9ex)
			{
				DX_CHECK(m_device->QueryInterface(IID_IDirect3DDevice9Ex, (void**)&m_deviceEx) );
			}
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX

			DX_CHECK(m_device->GetDeviceCaps(&m_caps) );

			// For shit GPUs that can create DX9 device but can't do simple stuff. GTFO!
			BX_WARN( (D3DPTEXTURECAPS_SQUAREONLY & m_caps.TextureCaps) == 0, "D3DPTEXTURECAPS_SQUAREONLY");
			BX_WARN( (D3DPTEXTURECAPS_MIPMAP     & m_caps.TextureCaps) == D3DPTEXTURECAPS_MIPMAP, "D3DPTEXTURECAPS_MIPMAP");
			BX_WARN( (D3DPTEXTURECAPS_ALPHA      & m_caps.TextureCaps) == D3DPTEXTURECAPS_ALPHA, "D3DPTEXTURECAPS_ALPHA");
			BX_WARN(m_caps.VertexShaderVersion >= D3DVS_VERSION(2, 0) && m_caps.PixelShaderVersion >= D3DPS_VERSION(2, 1)
					  , "Shader Model Version (vs: %x, ps: %x)."
					  , m_caps.VertexShaderVersion
					  , m_caps.PixelShaderVersion
					  );

			if ( (D3DPTEXTURECAPS_SQUAREONLY & m_caps.TextureCaps) != 0
			||   (D3DPTEXTURECAPS_MIPMAP     & m_caps.TextureCaps) != D3DPTEXTURECAPS_MIPMAP
			||   (D3DPTEXTURECAPS_ALPHA      & m_caps.TextureCaps) != D3DPTEXTURECAPS_ALPHA
			||   !(m_caps.VertexShaderVersion >= D3DVS_VERSION(2, 0) && m_caps.PixelShaderVersion >= D3DPS_VERSION(2, 1) ) )
			{
				goto error;
			}

			BX_TRACE("Max vertex shader 3.0 instr. slots: %d", m_caps.MaxVertexShader30InstructionSlots);
			BX_TRACE("Max vertex shader constants: %d", m_caps.MaxVertexShaderConst);
			BX_TRACE("Max fragment shader 2.0 instr. slots: %d", m_caps.PS20Caps.NumInstructionSlots);
			BX_TRACE("Max fragment shader 3.0 instr. slots: %d", m_caps.MaxPixelShader30InstructionSlots);
			BX_TRACE("Num simultaneous render targets: %d", m_caps.NumSimultaneousRTs);
			BX_TRACE("Max vertex index: %d", m_caps.MaxVertexIndex);

			g_caps.supported |= ( 0
								| BGFX_CAPS_TEXTURE_3D
								| BGFX_CAPS_TEXTURE_COMPARE_LEQUAL
								| BGFX_CAPS_VERTEX_ATTRIB_HALF
								| BGFX_CAPS_VERTEX_ATTRIB_UINT10
								| BGFX_CAPS_FRAGMENT_DEPTH
								| BGFX_CAPS_SWAP_CHAIN
								| ( (UINT16_MAX < m_caps.MaxVertexIndex) ? BGFX_CAPS_INDEX32 : 0)
								);
			g_caps.maxTextureSize = uint16_t(bx::uint32_min(m_caps.MaxTextureWidth, m_caps.MaxTextureHeight) );
//			g_caps.maxVertexIndex = m_caps.MaxVertexIndex;

			m_caps.NumSimultaneousRTs = uint8_t(bx::uint32_min(m_caps.NumSimultaneousRTs, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS) );
			g_caps.maxFBAttachments   = uint8_t(m_caps.NumSimultaneousRTs);

			m_caps.MaxAnisotropy = bx::uint32_max(m_caps.MaxAnisotropy, 1);

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_USE_EXTENSIONS) )
			{
				BX_TRACE("Extended formats:");
				for (uint32_t ii = 0; ii < ExtendedFormat::Count; ++ii)
				{
					ExtendedFormat& fmt = s_extendedFormats[ii];
					fmt.m_supported = SUCCEEDED(m_d3d9->CheckDeviceFormat(m_adapter, m_deviceType, adapterFormat, fmt.m_usage, fmt.m_type, fmt.m_fmt) );
					const char* fourcc = (const char*)&fmt.m_fmt;
					BX_TRACE("\t%2d: %c%c%c%c %s", ii, fourcc[0], fourcc[1], fourcc[2], fourcc[3], fmt.m_supported ? "supported" : "");
					BX_UNUSED(fourcc);
				}

				m_instancingSupport = false
					|| s_extendedFormats[ExtendedFormat::Inst].m_supported
					|| (m_caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) )
					;

				if (m_amd
				&&  s_extendedFormats[ExtendedFormat::Inst].m_supported)
				{   // AMD only
					m_device->SetRenderState(D3DRS_POINTSIZE, D3DFMT_INST);
				}

				if (s_extendedFormats[ExtendedFormat::Intz].m_supported)
				{
					s_textureFormat[TextureFormat::D24].m_fmt = D3DFMT_INTZ;
					s_textureFormat[TextureFormat::D32].m_fmt = D3DFMT_INTZ;
				}

				s_textureFormat[TextureFormat::BC4].m_fmt = s_extendedFormats[ExtendedFormat::Ati1].m_supported ? D3DFMT_ATI1 : D3DFMT_UNKNOWN;
				s_textureFormat[TextureFormat::BC5].m_fmt = s_extendedFormats[ExtendedFormat::Ati2].m_supported ? D3DFMT_ATI2 : D3DFMT_UNKNOWN;

				g_caps.supported |= m_instancingSupport ? BGFX_CAPS_INSTANCING : 0;

				for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
				{
					uint8_t support = SUCCEEDED(m_d3d9->CheckDeviceFormat(m_adapter
						, m_deviceType
						, adapterFormat
						, 0
						, D3DRTYPE_TEXTURE
						, s_textureFormat[ii].m_fmt
						) ) ? BGFX_CAPS_FORMAT_TEXTURE_COLOR : BGFX_CAPS_FORMAT_TEXTURE_NONE;

					support |= SUCCEEDED(m_d3d9->CheckDeviceFormat(m_adapter
						, m_deviceType
						, adapterFormat
						, D3DUSAGE_QUERY_SRGBREAD
						, D3DRTYPE_TEXTURE
						, s_textureFormat[ii].m_fmt
						) ) ? BGFX_CAPS_FORMAT_TEXTURE_COLOR_SRGB : BGFX_CAPS_FORMAT_TEXTURE_NONE;

					support |= SUCCEEDED(m_d3d9->CheckDeviceFormat(m_adapter
						, m_deviceType
						, adapterFormat
						, D3DUSAGE_QUERY_VERTEXTEXTURE
						, D3DRTYPE_TEXTURE
						, s_textureFormat[ii].m_fmt
						) ) ? BGFX_CAPS_FORMAT_TEXTURE_VERTEX : BGFX_CAPS_FORMAT_TEXTURE_NONE;

					support |= SUCCEEDED(m_d3d9->CheckDeviceFormat(m_adapter
						, m_deviceType
						, adapterFormat
						, isDepth(TextureFormat::Enum(ii) ) ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET
						, D3DRTYPE_TEXTURE
						, s_textureFormat[ii].m_fmt
						) ) ? BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER : BGFX_CAPS_FORMAT_TEXTURE_NONE;

					g_caps.formats[ii] = support;
				}
			}

			{
				uint32_t index = 1;
				for (const D3DFORMAT* fmt = &s_checkColorFormats[index]; *fmt != D3DFMT_UNKNOWN; ++fmt, ++index)
				{
					for (; *fmt != D3DFMT_UNKNOWN; ++fmt)
					{
						if (SUCCEEDED(m_d3d9->CheckDeviceFormat(m_adapter, m_deviceType, adapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, *fmt) ) )
						{
							s_colorFormat[index] = *fmt;
							break;
						}
					}

					for (; *fmt != D3DFMT_UNKNOWN; ++fmt);
				}
			}

			m_fmtDepth = D3DFMT_D24S8;

#elif BX_PLATFORM_XBOX360
			m_params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
			m_params.DisableAutoBackBuffer = FALSE;
			m_params.DisableAutoFrontBuffer = FALSE;
			m_params.FrontBufferFormat = D3DFMT_X8R8G8B8;
			m_params.FrontBufferColorSpace = D3DCOLORSPACE_RGB;

			m_d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
			BX_TRACE("Creating D3D9 %p", m_d3d9);

			XVIDEO_MODE videoMode;
			XGetVideoMode(&videoMode);
			if (!videoMode.fIsWideScreen)
			{
				m_params.Flags |= D3DPRESENTFLAG_NO_LETTERBOX;
			}

			BX_TRACE("Creating device");
			DX_CHECK(m_d3d9->CreateDevice(m_adapter
					, m_deviceType
					, NULL
					, D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_BUFFER_2_FRAMES
					, &m_params
					, &m_device
					) );

			BX_TRACE("Device %p", m_device);

			m_fmtDepth = D3DFMT_D24FS8;
#endif // BX_PLATFORM_WINDOWS

			{
				IDirect3DQuery9* timerQueryTest[3] = {};
				m_timerQuerySupport = true
					&& SUCCEEDED(m_device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &timerQueryTest[0]) )
					&& SUCCEEDED(m_device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,         &timerQueryTest[1]) )
					&& SUCCEEDED(m_device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,     &timerQueryTest[2]) )
					;
				DX_RELEASE(timerQueryTest[0], 0);
				DX_RELEASE(timerQueryTest[1], 0);
				DX_RELEASE(timerQueryTest[2], 0);
			}

			{
				IDirect3DSwapChain9* swapChain;
				DX_CHECK(m_device->GetSwapChain(0, &swapChain) );

				// GPA increases swapchain ref count.
				//
				// This causes assert in debug. When debugger is present refcount
				// checks are off.
				setGraphicsDebuggerPresent(1 != getRefCount(swapChain) );

				DX_RELEASE(swapChain, 0);
			}

			// Init reserved part of view name.
			for (uint32_t ii = 0; ii < BGFX_CONFIG_MAX_VIEWS; ++ii)
			{
				char name[BGFX_CONFIG_MAX_VIEW_NAME_RESERVED+1];
				bx::snprintf(name, sizeof(name), "%3d   ", ii);
				mbstowcs(s_viewNameW[ii], name, BGFX_CONFIG_MAX_VIEW_NAME_RESERVED);
			}

			postReset();

			m_initialized = true;

			return true;

		error:
			switch (errorState)
			{
			case ErrorState::CreatedDevice:
#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
				if (NULL != m_d3d9ex)
				{
					DX_RELEASE(m_deviceEx, 1);
					DX_RELEASE(m_device, 0);
				}
				else
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX
				{
					DX_RELEASE(m_device, 0);
				}

			case ErrorState::CreatedD3D9:
#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
				if (NULL != m_d3d9ex)
				{
					DX_RELEASE(m_d3d9, 1);
					DX_RELEASE(m_d3d9ex, 0);
				}
				else
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX
				{
					DX_RELEASE(m_d3d9, 0);
				}

#if BX_PLATFORM_WINDOWS
			case ErrorState::LoadedD3D9:
				bx::dlclose(m_d3d9dll);
#endif // BX_PLATFORM_WINDOWS

			case ErrorState::Default:
				break;
			}

			return false;
		}

		void shutdown()
		{
			preReset();

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_indexBuffers); ++ii)
			{
				m_indexBuffers[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_vertexBuffers); ++ii)
			{
				m_vertexBuffers[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_shaders); ++ii)
			{
				m_shaders[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_textures); ++ii)
			{
				m_textures[ii].destroy();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_vertexDecls); ++ii)
			{
				m_vertexDecls[ii].destroy();
			}

#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
			if (NULL != m_d3d9ex)
			{
				DX_RELEASE(m_deviceEx, 1);
				DX_RELEASE(m_device, 0);
				DX_RELEASE(m_d3d9, 1);
				DX_RELEASE(m_d3d9ex, 0);
			}
			else
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX
			{
				DX_RELEASE(m_device, 0);
				DX_RELEASE(m_d3d9, 0);
			}

#if BX_PLATFORM_WINDOWS
			bx::dlclose(m_d3d9dll);
#endif // BX_PLATFORM_WINDOWS

			m_initialized = false;
		}

		RendererType::Enum getRendererType() const BX_OVERRIDE
		{
			return RendererType::Direct3D9;
		}

		const char* getRendererName() const BX_OVERRIDE
		{
			return BGFX_RENDERER_DIRECT3D9_NAME;
		}

		void createIndexBuffer(IndexBufferHandle _handle, Memory* _mem, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_mem->size, _mem->data, _flags);
		}

		void destroyIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createVertexDecl(VertexDeclHandle _handle, const VertexDecl& _decl) BX_OVERRIDE
		{
			m_vertexDecls[_handle.idx].create(_decl);
		}

		void destroyVertexDecl(VertexDeclHandle _handle) BX_OVERRIDE
		{
			m_vertexDecls[_handle.idx].destroy();
		}

		void createVertexBuffer(VertexBufferHandle _handle, Memory* _mem, VertexDeclHandle _declHandle, uint16_t /*_flags*/) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].create(_mem->size, _mem->data, _declHandle);
		}

		void destroyVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_size, NULL, _flags);
		}

		void updateDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].update(_offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _size, uint16_t /*_flags*/) BX_OVERRIDE
		{
			VertexDeclHandle decl = BGFX_INVALID_HANDLE;
			m_vertexBuffers[_handle.idx].create(_size, NULL, decl);
		}

		void updateDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].update(_offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createShader(ShaderHandle _handle, Memory* _mem) BX_OVERRIDE
		{
			m_shaders[_handle.idx].create(_mem);
		}

		void destroyShader(ShaderHandle _handle) BX_OVERRIDE
		{
			m_shaders[_handle.idx].destroy();
		}

		void createProgram(ProgramHandle _handle, ShaderHandle _vsh, ShaderHandle _fsh) BX_OVERRIDE
		{
			m_program[_handle.idx].create(m_shaders[_vsh.idx], m_shaders[_fsh.idx]);
		}

		void destroyProgram(ProgramHandle _handle) BX_OVERRIDE
		{
			m_program[_handle.idx].destroy();
		}

		void createTexture(TextureHandle _handle, Memory* _mem, uint32_t _flags, uint8_t _skip) BX_OVERRIDE
		{
			m_textures[_handle.idx].create(_mem, _flags, _skip);
		}

		void updateTextureBegin(TextureHandle _handle, uint8_t _side, uint8_t _mip) BX_OVERRIDE
		{
			m_updateTexture = &m_textures[_handle.idx];
			m_updateTexture->updateBegin(_side, _mip);
		}

		void updateTexture(TextureHandle /*_handle*/, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem) BX_OVERRIDE
		{
			m_updateTexture->update(_side, _mip, _rect, _z, _depth, _pitch, _mem);
		}

		void updateTextureEnd() BX_OVERRIDE
		{
			m_updateTexture->updateEnd();
			m_updateTexture = NULL;
		}

		void resizeTexture(TextureHandle _handle, uint16_t _width, uint16_t _height) BX_OVERRIDE
		{
			TextureD3D9& texture = m_textures[_handle.idx];
			texture.m_width  = _width;
			texture.m_height = _height;
		}

		void destroyTexture(TextureHandle _handle) BX_OVERRIDE
		{
			m_textures[_handle.idx].destroy();
		}

		void createFrameBuffer(FrameBufferHandle _handle, uint8_t _num, const TextureHandle* _textureHandles) BX_OVERRIDE
		{
			m_frameBuffers[_handle.idx].create(_num, _textureHandles);
		}

		void createFrameBuffer(FrameBufferHandle _handle, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat) BX_OVERRIDE
		{
			uint16_t denseIdx = m_numWindows++;
			m_windows[denseIdx] = _handle;
			m_frameBuffers[_handle.idx].create(denseIdx, _nwh, _width, _height, _depthFormat);
		}

		void destroyFrameBuffer(FrameBufferHandle _handle) BX_OVERRIDE
		{
			uint16_t denseIdx = m_frameBuffers[_handle.idx].destroy();
			if (UINT16_MAX != denseIdx)
			{
				--m_numWindows;
				if (m_numWindows > 1)
				{
					FrameBufferHandle handle = m_windows[m_numWindows];
					m_windows[denseIdx] = handle;
					m_frameBuffers[handle.idx].m_denseIdx = denseIdx;
				}
			}
		}

		void createUniform(UniformHandle _handle, UniformType::Enum _type, uint16_t _num, const char* _name) BX_OVERRIDE
		{
			if (NULL != m_uniforms[_handle.idx])
			{
				BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			}

			uint32_t size = BX_ALIGN_16(g_uniformTypeSize[_type]*_num);
			void* data = BX_ALLOC(g_allocator, size);
			memset(data, 0, size);
			m_uniforms[_handle.idx] = data;
			m_uniformReg.add(_handle, _name, data);
		}

		void destroyUniform(UniformHandle _handle) BX_OVERRIDE
		{
			BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			m_uniforms[_handle.idx] = NULL;
		}

		void saveScreenShot(const char* _filePath) BX_OVERRIDE
		{
#if BX_PLATFORM_WINDOWS
			IDirect3DSurface9* surface;
			D3DDEVICE_CREATION_PARAMETERS dcp;
			DX_CHECK(m_device->GetCreationParameters(&dcp) );

			D3DDISPLAYMODE dm;
			DX_CHECK(m_d3d9->GetAdapterDisplayMode(dcp.AdapterOrdinal, &dm) );

			DX_CHECK(m_device->CreateOffscreenPlainSurface(dm.Width
				, dm.Height
				, D3DFMT_A8R8G8B8
				, D3DPOOL_SCRATCH
				, &surface
				, NULL
				) );

			DX_CHECK(m_device->GetFrontBufferData(0, surface) );

			D3DLOCKED_RECT rect;
			DX_CHECK(surface->LockRect(&rect
				, NULL
				, D3DLOCK_NO_DIRTY_UPDATE|D3DLOCK_NOSYSLOCK|D3DLOCK_READONLY
				) );

			RECT rc;
			GetClientRect( (HWND)g_platformData.nwh, &rc);
			POINT point;
			point.x = rc.left;
			point.y = rc.top;
			ClientToScreen( (HWND)g_platformData.nwh, &point);
			uint8_t* data = (uint8_t*)rect.pBits;
			uint32_t bytesPerPixel = rect.Pitch/dm.Width;

			g_callback->screenShot(_filePath
				, m_params.BackBufferWidth
				, m_params.BackBufferHeight
				, rect.Pitch
				, &data[point.y*rect.Pitch+point.x*bytesPerPixel]
				, m_params.BackBufferHeight*rect.Pitch
				, false
				);

			DX_CHECK(surface->UnlockRect() );
			DX_RELEASE(surface, 0);
#endif // BX_PLATFORM_WINDOWS
		}

		void updateViewName(uint8_t _id, const char* _name) BX_OVERRIDE
		{
			if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
			{
				mbstowcs(&s_viewNameW[_id][BGFX_CONFIG_MAX_VIEW_NAME_RESERVED]
					, _name
					, BX_COUNTOF(s_viewNameW[0])-BGFX_CONFIG_MAX_VIEW_NAME_RESERVED
					);
			}
		}

		void updateUniform(uint16_t _loc, const void* _data, uint32_t _size) BX_OVERRIDE
		{
			memcpy(m_uniforms[_loc], _data, _size);
		}

		void setMarker(const char* _marker, uint32_t _size) BX_OVERRIDE
		{
#if BGFX_CONFIG_DEBUG_PIX
			uint32_t size = _size*sizeof(wchar_t);
			wchar_t* name = (wchar_t*)alloca(size);
			mbstowcs(name, _marker, size-2);
			PIX_SETMARKER(D3DCOLOR_RGBA(0xff, 0xff, 0xff, 0xff), name);
#endif // BGFX_CONFIG_DEBUG_PIX
			BX_UNUSED(_marker, _size);
		}

		void submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter) BX_OVERRIDE;

		void blitSetup(TextVideoMemBlitter& _blitter) BX_OVERRIDE
		{
			uint32_t width  = m_params.BackBufferWidth;
			uint32_t height = m_params.BackBufferHeight;

			FrameBufferHandle fbh = BGFX_INVALID_HANDLE;
			setFrameBuffer(fbh, false);

			D3DVIEWPORT9 vp;
			vp.X = 0;
			vp.Y = 0;
			vp.Width = width;
			vp.Height = height;
			vp.MinZ = 0.0f;
			vp.MaxZ = 1.0f;

			IDirect3DDevice9* device = m_device;
			DX_CHECK(device->SetViewport(&vp) );
			DX_CHECK(device->SetRenderState(D3DRS_STENCILENABLE, FALSE) );
			DX_CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE) );
			DX_CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS) );
			DX_CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE) );
			DX_CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE) );
			DX_CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER) );
			DX_CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE) );
			DX_CHECK(device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID) );

			ProgramD3D9& program = m_program[_blitter.m_program.idx];
			DX_CHECK(device->SetVertexShader(program.m_vsh->m_vertexShader) );
			DX_CHECK(device->SetPixelShader(program.m_fsh->m_pixelShader) );

			VertexBufferD3D9& vb = m_vertexBuffers[_blitter.m_vb->handle.idx];
			VertexDeclD3D9& vertexDecl = m_vertexDecls[_blitter.m_vb->decl.idx];
			DX_CHECK(device->SetStreamSource(0, vb.m_ptr, 0, vertexDecl.m_decl.m_stride) );
			DX_CHECK(device->SetVertexDeclaration(vertexDecl.m_ptr) );

			IndexBufferD3D9& ib = m_indexBuffers[_blitter.m_ib->handle.idx];
			DX_CHECK(device->SetIndices(ib.m_ptr) );

			float proj[16];
			bx::mtxOrtho(proj, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1000.0f);

			PredefinedUniform& predefined = program.m_predefined[0];
			uint8_t flags = predefined.m_type;
			setShaderUniform(flags, predefined.m_loc, proj, 4);

			m_textures[_blitter.m_texture.idx].commit(0);
		}

		void blitRender(TextVideoMemBlitter& _blitter, uint32_t _numIndices) BX_OVERRIDE
		{
			const uint32_t numVertices = _numIndices*4/6;
			if (0 < numVertices)
			{
				m_indexBuffers[_blitter.m_ib->handle.idx].update(0, _numIndices * 2, _blitter.m_ib->data, true);
				m_vertexBuffers[_blitter.m_vb->handle.idx].update(0, numVertices*_blitter.m_decl.m_stride, _blitter.m_vb->data, true);

				DX_CHECK(m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST
					, 0
					, 0
					, numVertices
					, 0
					, _numIndices / 3
					) );
			}
		}

		void updateMsaa()
		{
			for (uint32_t ii = 1, last = 0; ii < BX_COUNTOF(s_checkMsaa); ++ii)
			{
				D3DMULTISAMPLE_TYPE msaa = s_checkMsaa[ii];
				DWORD quality;

				HRESULT hr = m_d3d9->CheckDeviceMultiSampleType(m_adapter
					, m_deviceType
					, m_params.BackBufferFormat
					, m_params.Windowed
					, msaa
					, &quality
					);

				if (SUCCEEDED(hr) )
				{
					s_msaa[ii].m_type = msaa;
					s_msaa[ii].m_quality = bx::uint32_imax(0, quality-1);
					last = ii;
				}
				else
				{
					s_msaa[ii] = s_msaa[last];
				}
			}
		}

		void updateResolution(const Resolution& _resolution)
		{
			m_maxAnisotropy = !!(_resolution.m_flags & BGFX_RESET_MAXANISOTROPY)
				? m_caps.MaxAnisotropy
				: 1
				;
			uint32_t flags = _resolution.m_flags & ~(BGFX_RESET_HMD_RECENTER | BGFX_RESET_MAXANISOTROPY);

			if (m_params.BackBufferWidth  != _resolution.m_width
			||  m_params.BackBufferHeight != _resolution.m_height
			||  m_flags != flags)
			{
				m_flags = flags;

				m_textVideoMem.resize(false, _resolution.m_width, _resolution.m_height);
				m_textVideoMem.clear();

#if BX_PLATFORM_WINDOWS
				D3DDEVICE_CREATION_PARAMETERS dcp;
				DX_CHECK(m_device->GetCreationParameters(&dcp) );

				D3DDISPLAYMODE dm;
				DX_CHECK(m_d3d9->GetAdapterDisplayMode(dcp.AdapterOrdinal, &dm) );

				m_params.BackBufferFormat = dm.Format;
#endif // BX_PLATFORM_WINDOWS

				m_params.BackBufferWidth  = _resolution.m_width;
				m_params.BackBufferHeight = _resolution.m_height;
				m_params.FullScreen_RefreshRateInHz = BGFX_RESET_FULLSCREEN == (m_flags&BGFX_RESET_FULLSCREEN_MASK) ? 60 : 0;
				m_params.PresentationInterval = !!(m_flags&BGFX_RESET_VSYNC) ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

				updateMsaa();

				Msaa& msaa = s_msaa[(m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT];
				m_params.MultiSampleType    = msaa.m_type;
				m_params.MultiSampleQuality = msaa.m_quality;

				m_resolution = _resolution;

				preReset();
				DX_CHECK(m_device->Reset(&m_params) );
				postReset();
			}
		}

		void setFrameBuffer(FrameBufferHandle _fbh, bool _msaa = true)
		{
			if (isValid(m_fbh)
			&&  m_fbh.idx != _fbh.idx
			&&  m_rtMsaa)
			{
				FrameBufferD3D9& frameBuffer = m_frameBuffers[m_fbh.idx];
				frameBuffer.resolve();
			}

			if (!isValid(_fbh) )
			{
				DX_CHECK(m_device->SetRenderTarget(0, m_backBufferColor) );
				for (uint32_t ii = 1, num = g_caps.maxFBAttachments; ii < num; ++ii)
				{
					DX_CHECK(m_device->SetRenderTarget(ii, NULL) );
				}
				DX_CHECK(m_device->SetDepthStencilSurface(m_backBufferDepthStencil) );

				DX_CHECK(m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, 0 != (m_flags & BGFX_RESET_SRGB_BACKBUFFER) ) );
			}
			else
			{
				const FrameBufferD3D9& frameBuffer = m_frameBuffers[_fbh.idx];

				// If frame buffer has only depth attachment D3DFMT_NULL
				// render target is created.
				uint32_t fbnum = bx::uint32_max(1, frameBuffer.m_num);

				for (uint32_t ii = 0; ii < fbnum; ++ii)
				{
					DX_CHECK(m_device->SetRenderTarget(ii, frameBuffer.m_color[ii]) );
				}

				for (uint32_t ii = fbnum, num = g_caps.maxFBAttachments; ii < num; ++ii)
				{
					DX_CHECK(m_device->SetRenderTarget(ii, NULL) );
				}

				IDirect3DSurface9* depthStencil = frameBuffer.m_depthStencil;
				DX_CHECK(m_device->SetDepthStencilSurface(NULL != depthStencil ? depthStencil : m_backBufferDepthStencil) );

				DX_CHECK(m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE) );
			}

			m_fbh = _fbh;
			m_rtMsaa = _msaa;
		}

		void setShaderUniform(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			if (_flags&BGFX_UNIFORM_FRAGMENTBIT)
			{
				DX_CHECK(m_device->SetPixelShaderConstantF(_regIndex, (const float*)_val, _numRegs) );
			}
			else
			{
				DX_CHECK(m_device->SetVertexShaderConstantF(_regIndex, (const float*)_val, _numRegs) );
			}
		}

		void setShaderUniform4f(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			setShaderUniform(_flags, _regIndex, _val, _numRegs);
		}

		void setShaderUniform4x4f(uint8_t _flags, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			setShaderUniform(_flags, _regIndex, _val, _numRegs);
		}

		void reset()
		{
			preReset();

			HRESULT hr;

			do
			{
				hr = m_device->Reset(&m_params);
			} while (FAILED(hr) );

			postReset();
		}

		static bool isLost(HRESULT _hr)
		{
			return D3DERR_DEVICELOST == _hr
				|| D3DERR_DRIVERINTERNALERROR == _hr
#if !defined(D3D_DISABLE_9EX)
				|| D3DERR_DEVICEHUNG == _hr
				|| D3DERR_DEVICEREMOVED == _hr
#endif // !defined(D3D_DISABLE_9EX)
				;
		}

		void flip(HMD& /*_hmd*/) BX_OVERRIDE
		{
			if (NULL != m_swapChain)
			{
#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
				if (NULL != m_deviceEx)
				{
					DX_CHECK(m_deviceEx->WaitForVBlank(0) );
				}
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX

				for (uint32_t ii = 0, num = m_numWindows; ii < num; ++ii)
				{
					HRESULT hr;
					if (0 == ii)
					{
						hr = m_swapChain->Present(NULL, NULL, (HWND)g_platformData.nwh, NULL, 0);
					}
					else
					{
						hr = m_frameBuffers[m_windows[ii].idx].present();
					}

#if BX_PLATFORM_WINDOWS
					if (isLost(hr) )
					{
						do
						{
							do
							{
								hr = m_device->TestCooperativeLevel();
							}
							while (D3DERR_DEVICENOTRESET != hr);

							reset();
							hr = m_device->TestCooperativeLevel();
						}
						while (FAILED(hr) );

						break;
					}
					else if (FAILED(hr) )
					{
						BX_TRACE("Present failed with err 0x%08x.", hr);
					}
#endif // BX_PLATFORM_
				}
			}
		}

		void preReset()
		{
			invalidateSamplerState();

			for (uint32_t stage = 0; stage < BGFX_CONFIG_MAX_TEXTURE_SAMPLERS; ++stage)
			{
				DX_CHECK(m_device->SetTexture(stage, NULL) );
			}

			DX_CHECK(m_device->SetRenderTarget(0, m_backBufferColor) );
			for (uint32_t ii = 1, num = g_caps.maxFBAttachments; ii < num; ++ii)
			{
				DX_CHECK(m_device->SetRenderTarget(ii, NULL) );
			}
			DX_CHECK(m_device->SetDepthStencilSurface(m_backBufferDepthStencil) );
			DX_CHECK(m_device->SetVertexShader(NULL) );
			DX_CHECK(m_device->SetPixelShader(NULL) );
			DX_CHECK(m_device->SetStreamSource(0, NULL, 0, 0) );
			DX_CHECK(m_device->SetIndices(NULL) );

			DX_RELEASE(m_backBufferColor, 0);
			DX_RELEASE(m_backBufferDepthStencil, 0);
			DX_RELEASE(m_swapChain, 0);

			capturePreReset();

			DX_RELEASE(m_flushQuery, 0);
			if (m_timerQuerySupport)
			{
				m_gpuTimer.preReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_indexBuffers); ++ii)
			{
				m_indexBuffers[ii].preReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_vertexBuffers); ++ii)
			{
				m_vertexBuffers[ii].preReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
			{
				m_frameBuffers[ii].preReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_textures); ++ii)
			{
				m_textures[ii].preReset();
			}
		}

		void postReset()
		{
			DX_CHECK(m_device->GetSwapChain(0, &m_swapChain) );
			DX_CHECK(m_swapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &m_backBufferColor) );
			DX_CHECK(m_device->GetDepthStencilSurface(&m_backBufferDepthStencil) );

			DX_CHECK(m_device->CreateQuery(D3DQUERYTYPE_EVENT, &m_flushQuery) );
			if (m_timerQuerySupport)
			{
				m_gpuTimer.postReset();
			}

			capturePostReset();

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_indexBuffers); ++ii)
			{
				m_indexBuffers[ii].postReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_vertexBuffers); ++ii)
			{
				m_vertexBuffers[ii].postReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_textures); ++ii)
			{
				m_textures[ii].postReset();
			}

			for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
			{
				m_frameBuffers[ii].postReset();
			}
		}

		void invalidateSamplerState()
		{
			for (uint32_t stage = 0; stage < BGFX_CONFIG_MAX_TEXTURE_SAMPLERS; ++stage)
			{
				m_samplerFlags[stage] = UINT32_MAX;
			}
		}

		static void setSamplerState(IDirect3DDevice9* _device, DWORD _stage, D3DSAMPLERSTATETYPE _type,DWORD _value)
		{
			DX_CHECK(_device->SetSamplerState(                           _stage, _type, _value) );
			DX_CHECK(_device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0 + _stage, _type, _value) );
		}

		void setSamplerState(uint8_t _stage, uint32_t _flags)
		{
			const uint32_t flags = _flags&( (~BGFX_TEXTURE_RESERVED_MASK) | BGFX_TEXTURE_SAMPLER_BITS_MASK | BGFX_TEXTURE_SRGB);
			BX_CHECK(_stage < BX_COUNTOF(m_samplerFlags), "");
			if (m_samplerFlags[_stage] != flags)
			{
				m_samplerFlags[_stage] = flags;
				IDirect3DDevice9* device = m_device;
				D3DTEXTUREADDRESS tau = s_textureAddress[(_flags&BGFX_TEXTURE_U_MASK)>>BGFX_TEXTURE_U_SHIFT];
				D3DTEXTUREADDRESS tav = s_textureAddress[(_flags&BGFX_TEXTURE_V_MASK)>>BGFX_TEXTURE_V_SHIFT];
				D3DTEXTUREADDRESS taw = s_textureAddress[(_flags&BGFX_TEXTURE_W_MASK)>>BGFX_TEXTURE_W_SHIFT];
				D3DTEXTUREFILTERTYPE minFilter = s_textureFilter[(_flags&BGFX_TEXTURE_MIN_MASK)>>BGFX_TEXTURE_MIN_SHIFT];
				D3DTEXTUREFILTERTYPE magFilter = s_textureFilter[(_flags&BGFX_TEXTURE_MAG_MASK)>>BGFX_TEXTURE_MAG_SHIFT];
				D3DTEXTUREFILTERTYPE mipFilter = s_textureFilter[(_flags&BGFX_TEXTURE_MIP_MASK)>>BGFX_TEXTURE_MIP_SHIFT];

				setSamplerState(device, _stage, D3DSAMP_ADDRESSU,  tau);
				setSamplerState(device, _stage, D3DSAMP_ADDRESSU,  tau);
				setSamplerState(device, _stage, D3DSAMP_ADDRESSV,  tav);
				setSamplerState(device, _stage, D3DSAMP_ADDRESSW,  taw);
				setSamplerState(device, _stage, D3DSAMP_MINFILTER, minFilter);
				setSamplerState(device, _stage, D3DSAMP_MAGFILTER, magFilter);
				setSamplerState(device, _stage, D3DSAMP_MIPFILTER, mipFilter);
				setSamplerState(device, _stage, D3DSAMP_MAXANISOTROPY, m_maxAnisotropy);
				setSamplerState(device, _stage, D3DSAMP_SRGBTEXTURE, 0 != (flags & BGFX_TEXTURE_SRGB) );
			}
		}

		void capturePreReset()
		{
			if (NULL != m_captureSurface)
			{
				g_callback->captureEnd();
			}
			DX_RELEASE(m_captureSurface, 1);
			DX_RELEASE(m_captureTexture, 0);
			DX_RELEASE(m_captureResolve, 0);
		}

		void capturePostReset()
		{
			if (m_flags&BGFX_RESET_CAPTURE)
			{
				uint32_t width  = m_params.BackBufferWidth;
				uint32_t height = m_params.BackBufferHeight;
				D3DFORMAT fmt   = m_params.BackBufferFormat;

				DX_CHECK(m_device->CreateTexture(width
					, height
					, 1
					, 0
					, fmt
					, D3DPOOL_SYSTEMMEM
					, &m_captureTexture
					, NULL
					) );

				DX_CHECK(m_captureTexture->GetSurfaceLevel(0
					, &m_captureSurface
					) );

				if (m_params.MultiSampleType != D3DMULTISAMPLE_NONE)
				{
					DX_CHECK(m_device->CreateRenderTarget(width
						, height
						, fmt
						, D3DMULTISAMPLE_NONE
						, 0
						, false
						, &m_captureResolve
						, NULL
						) );
				}

				g_callback->captureBegin(width, height, width*4, TextureFormat::BGRA8, false);
			}
		}

		void capture()
		{
			if (NULL != m_captureSurface)
			{
				IDirect3DSurface9* resolve = m_backBufferColor;

				if (NULL != m_captureResolve)
				{
					resolve = m_captureResolve;
					DX_CHECK(m_device->StretchRect(m_backBufferColor
						, 0
						, m_captureResolve
						, NULL
						, D3DTEXF_NONE
						) );
				}

				HRESULT hr = m_device->GetRenderTargetData(resolve, m_captureSurface);
				if (SUCCEEDED(hr) )
				{
					D3DLOCKED_RECT rect;
					DX_CHECK(m_captureSurface->LockRect(&rect
						, NULL
						, D3DLOCK_NO_DIRTY_UPDATE|D3DLOCK_NOSYSLOCK|D3DLOCK_READONLY
						) );

					g_callback->captureFrame(rect.pBits, m_params.BackBufferHeight*rect.Pitch);

					DX_CHECK(m_captureSurface->UnlockRect() );
				}
			}
		}

		void commit(ConstantBuffer& _constantBuffer)
		{
			_constantBuffer.reset();

			IDirect3DDevice9* device = m_device;

			for (;;)
			{
				uint32_t opcode = _constantBuffer.read();

				if (UniformType::End == opcode)
				{
					break;
				}

				UniformType::Enum type;
				uint16_t loc;
				uint16_t num;
				uint16_t copy;
				ConstantBuffer::decodeOpcode(opcode, type, loc, num, copy);

				const char* data;
				if (copy)
				{
					data = _constantBuffer.read(g_uniformTypeSize[type]*num);
				}
				else
				{
					UniformHandle handle;
					memcpy(&handle, _constantBuffer.read(sizeof(UniformHandle) ), sizeof(UniformHandle) );
					data = (const char*)m_uniforms[handle.idx];
				}

#define CASE_IMPLEMENT_UNIFORM(_uniform, _dxsuffix, _type) \
				case UniformType::_uniform: \
				{ \
					_type* value = (_type*)data; \
					DX_CHECK(device->SetVertexShaderConstant##_dxsuffix(loc, value, num) ); \
				} \
				break; \
				\
				case UniformType::_uniform|BGFX_UNIFORM_FRAGMENTBIT: \
				{ \
					_type* value = (_type*)data; \
					DX_CHECK(device->SetPixelShaderConstant##_dxsuffix(loc, value, num) ); \
				} \
				break

				switch ( (int32_t)type)
				{
				case UniformType::Mat3:
					{
						float* value = (float*)data;
						for (uint32_t ii = 0, count = num/3; ii < count; ++ii,  loc += 3, value += 9)
						{
							Matrix4 mtx;
							mtx.un.val[ 0] = value[0];
							mtx.un.val[ 1] = value[1];
							mtx.un.val[ 2] = value[2];
							mtx.un.val[ 3] = 0.0f;
							mtx.un.val[ 4] = value[3];
							mtx.un.val[ 5] = value[4];
							mtx.un.val[ 6] = value[5];
							mtx.un.val[ 7] = 0.0f;
							mtx.un.val[ 8] = value[6];
							mtx.un.val[ 9] = value[7];
							mtx.un.val[10] = value[8];
							mtx.un.val[11] = 0.0f;
							DX_CHECK(device->SetVertexShaderConstantF(loc, &mtx.un.val[0], 3) );
						}
					}
					break;

				case UniformType::Mat3|BGFX_UNIFORM_FRAGMENTBIT:
					{
						float* value = (float*)data;
						for (uint32_t ii = 0, count = num/3; ii < count; ++ii, loc += 3, value += 9)
						{
							Matrix4 mtx;
							mtx.un.val[ 0] = value[0];
							mtx.un.val[ 1] = value[1];
							mtx.un.val[ 2] = value[2];
							mtx.un.val[ 3] = 0.0f;
							mtx.un.val[ 4] = value[3];
							mtx.un.val[ 5] = value[4];
							mtx.un.val[ 6] = value[5];
							mtx.un.val[ 7] = 0.0f;
							mtx.un.val[ 8] = value[6];
							mtx.un.val[ 9] = value[7];
							mtx.un.val[10] = value[8];
							mtx.un.val[11] = 0.0f;
							DX_CHECK(device->SetPixelShaderConstantF(loc, &mtx.un.val[0], 3) );
						}
					}
					break;

				CASE_IMPLEMENT_UNIFORM(Int1, I, int);
				CASE_IMPLEMENT_UNIFORM(Vec4, F, float);
				CASE_IMPLEMENT_UNIFORM(Mat4, F, float);

				case UniformType::End:
					break;

				default:
					BX_TRACE("%4d: INVALID 0x%08x, t %d, l %d, n %d, c %d", _constantBuffer.getPos(), opcode, type, loc, num, copy);
					break;
				}
#undef CASE_IMPLEMENT_UNIFORM
			}
		}

		void clearQuad(ClearQuad& _clearQuad, const Rect& _rect, const Clear& _clear, const float _palette[][4])
		{
			IDirect3DDevice9* device = m_device;

			uint32_t numMrt = 1;
			FrameBufferHandle fbh = m_fbh;
			if (isValid(fbh) )
			{
				const FrameBufferD3D9& fb = m_frameBuffers[fbh.idx];
				numMrt = bx::uint32_max(1, fb.m_num);
			}

			if (1 == numMrt)
			{
				D3DCOLOR color = 0;
				DWORD flags    = 0;

				if (BGFX_CLEAR_COLOR & _clear.m_flags)
				{
					if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
					{
						uint8_t index = (uint8_t)bx::uint32_min(BGFX_CONFIG_MAX_CLEAR_COLOR_PALETTE-1, _clear.m_index[0]);
						const float* rgba = _palette[index];
						const float rr = rgba[0];
						const float gg = rgba[1];
						const float bb = rgba[2];
						const float aa = rgba[3];
						color = D3DCOLOR_COLORVALUE(rr, gg, bb, aa);
					}
					else
					{
						color = D3DCOLOR_RGBA(_clear.m_index[0], _clear.m_index[1], _clear.m_index[2], _clear.m_index[3]);
					}

					flags |= D3DCLEAR_TARGET;
					DX_CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE
						, D3DCOLORWRITEENABLE_RED
						| D3DCOLORWRITEENABLE_GREEN
						| D3DCOLORWRITEENABLE_BLUE
						| D3DCOLORWRITEENABLE_ALPHA
						) );
				}

				if (BGFX_CLEAR_DEPTH & _clear.m_flags)
				{
					flags |= D3DCLEAR_ZBUFFER;
					DX_CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE) );
				}

				if (BGFX_CLEAR_STENCIL & _clear.m_flags)
				{
					flags |= D3DCLEAR_STENCIL;
				}

				if (0 != flags)
				{
					RECT rc;
					rc.left   = _rect.m_x;
					rc.top    = _rect.m_y;
					rc.right  = _rect.m_x + _rect.m_width;
					rc.bottom = _rect.m_y + _rect.m_height;
					DX_CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE) );
					DX_CHECK(device->SetScissorRect(&rc) );
					DX_CHECK(device->Clear(0, NULL, flags, color, _clear.m_depth, _clear.m_stencil) );
					DX_CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE) );
				}
			}
			else
			{
				DX_CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE) );
				DX_CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE) );
				DX_CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE) );

				if (BGFX_CLEAR_COLOR & _clear.m_flags)
				{
					DX_CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE
						, D3DCOLORWRITEENABLE_RED
						| D3DCOLORWRITEENABLE_GREEN
						| D3DCOLORWRITEENABLE_BLUE
						| D3DCOLORWRITEENABLE_ALPHA
						) );
				}
				else
				{
					DX_CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, 0) );
				}

				if (BGFX_CLEAR_DEPTH & _clear.m_flags)
				{
					DX_CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE) );
					DX_CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE) );
					DX_CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS) );
				}
				else
				{
					DX_CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE) );
					DX_CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE) );
				}

				if (BGFX_CLEAR_STENCIL & _clear.m_flags)
				{
					DX_CHECK(device->SetRenderState(D3DRS_STENCILENABLE, TRUE) );
					DX_CHECK(device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, TRUE) );
					DX_CHECK(device->SetRenderState(D3DRS_STENCILREF, _clear.m_stencil) );
					DX_CHECK(device->SetRenderState(D3DRS_STENCILMASK, 0xff) );
					DX_CHECK(device->SetRenderState(D3DRS_STENCILFUNC,  D3DCMP_ALWAYS) );
					DX_CHECK(device->SetRenderState(D3DRS_STENCILFAIL,  D3DSTENCILOP_REPLACE) );
					DX_CHECK(device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_REPLACE) );
					DX_CHECK(device->SetRenderState(D3DRS_STENCILPASS,  D3DSTENCILOP_REPLACE) );
				}
				else
				{
					DX_CHECK(device->SetRenderState(D3DRS_STENCILENABLE, FALSE) );
				}

				VertexBufferD3D9& vb = m_vertexBuffers[_clearQuad.m_vb->handle.idx];
				VertexDeclD3D9& vertexDecl = m_vertexDecls[_clearQuad.m_vb->decl.idx];
				uint32_t stride = _clearQuad.m_decl.m_stride;

				{
					struct Vertex
					{
						float m_x;
						float m_y;
						float m_z;
					};

					Vertex* vertex = (Vertex*)_clearQuad.m_vb->data;
					BX_CHECK(stride == sizeof(Vertex), "Stride/Vertex mismatch (stride %d, sizeof(Vertex) %d)", stride, sizeof(Vertex) );

					const float depth = _clear.m_depth;

					vertex->m_x = -1.0f;
					vertex->m_y = -1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x =  1.0f;
					vertex->m_y = -1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x = -1.0f;
					vertex->m_y =  1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x =  1.0f;
					vertex->m_y =  1.0f;
					vertex->m_z = depth;
				}

				vb.update(0, 4*stride, _clearQuad.m_vb->data);

				ProgramD3D9& program = m_program[_clearQuad.m_program[numMrt-1].idx];
				device->SetVertexShader(program.m_vsh->m_vertexShader);
				device->SetPixelShader(program.m_fsh->m_pixelShader);

				if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
				{
					float mrtClear[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS][4];
					for (uint32_t ii = 0; ii < numMrt; ++ii)
					{
						uint8_t index = (uint8_t)bx::uint32_min(BGFX_CONFIG_MAX_CLEAR_COLOR_PALETTE-1, _clear.m_index[ii]);
						memcpy(mrtClear[ii], _palette[index], 16);
					}

					DX_CHECK(m_device->SetPixelShaderConstantF(0, mrtClear[0], numMrt) );
				}
				else
				{
					float rgba[4] =
					{
						_clear.m_index[0]*1.0f/255.0f,
						_clear.m_index[1]*1.0f/255.0f,
						_clear.m_index[2]*1.0f/255.0f,
						_clear.m_index[3]*1.0f/255.0f,
					};

					DX_CHECK(m_device->SetPixelShaderConstantF(0, rgba, 1) );
				}

				DX_CHECK(device->SetStreamSource(0, vb.m_ptr, 0, stride) );
				DX_CHECK(device->SetStreamSourceFreq(0, 1) );
				DX_CHECK(device->SetStreamSource(1, NULL, 0, 0) );
				DX_CHECK(device->SetVertexDeclaration(vertexDecl.m_ptr) );
				DX_CHECK(device->SetIndices(NULL) );
				DX_CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP
					, 0
					, 2
					) );
			}
		}

#if BX_PLATFORM_WINDOWS
		D3DCAPS9 m_caps;
#endif // BX_PLATFORM_WINDOWS

#if BGFX_CONFIG_RENDERER_DIRECT3D9EX
		IDirect3D9Ex* m_d3d9ex;
		IDirect3DDevice9Ex* m_deviceEx;
#endif // BGFX_CONFIG_RENDERER_DIRECT3D9EX

		IDirect3D9*       m_d3d9;
		IDirect3DDevice9* m_device;
		IDirect3DQuery9*  m_flushQuery;
		TimerQueryD3D9    m_gpuTimer;
		D3DPOOL m_pool;

		IDirect3DSwapChain9* m_swapChain;
		uint16_t m_numWindows;
		FrameBufferHandle m_windows[BGFX_CONFIG_MAX_FRAME_BUFFERS];

		IDirect3DSurface9* m_backBufferColor;
		IDirect3DSurface9* m_backBufferDepthStencil;

		IDirect3DTexture9* m_captureTexture;
		IDirect3DSurface9* m_captureSurface;
		IDirect3DSurface9* m_captureResolve;

		IDirect3DVertexDeclaration9* m_instanceDataDecls[BGFX_CONFIG_MAX_INSTANCE_DATA_COUNT];

		void* m_d3d9dll;
		uint32_t m_adapter;
		D3DDEVTYPE m_deviceType;
		D3DPRESENT_PARAMETERS m_params;
		uint32_t m_flags;
		uint32_t m_maxAnisotropy;
		D3DADAPTER_IDENTIFIER9 m_identifier;
		Resolution m_resolution;

		bool m_initialized;
		bool m_amd;
		bool m_nvidia;
		bool m_instancingSupport;
		bool m_timerQuerySupport;

		D3DFORMAT m_fmtDepth;

		IndexBufferD3D9 m_indexBuffers[BGFX_CONFIG_MAX_INDEX_BUFFERS];
		VertexBufferD3D9 m_vertexBuffers[BGFX_CONFIG_MAX_VERTEX_BUFFERS];
		ShaderD3D9 m_shaders[BGFX_CONFIG_MAX_SHADERS];
		ProgramD3D9 m_program[BGFX_CONFIG_MAX_PROGRAMS];
		TextureD3D9 m_textures[BGFX_CONFIG_MAX_TEXTURES];
		VertexDeclD3D9 m_vertexDecls[BGFX_CONFIG_MAX_VERTEX_DECLS];
		FrameBufferD3D9 m_frameBuffers[BGFX_CONFIG_MAX_FRAME_BUFFERS];
		UniformRegistry m_uniformReg;
		void* m_uniforms[BGFX_CONFIG_MAX_UNIFORMS];

		uint32_t m_samplerFlags[BGFX_CONFIG_MAX_TEXTURE_SAMPLERS];

		TextureD3D9* m_updateTexture;
		uint8_t* m_updateTextureBits;
		uint32_t m_updateTexturePitch;
		uint8_t m_updateTextureSide;
		uint8_t m_updateTextureMip;

		TextVideoMem m_textVideoMem;

		FrameBufferHandle m_fbh;
		bool m_rtMsaa;
	};

	static RendererContextD3D9* s_renderD3D9;

	RendererContextI* rendererCreate()
	{
		s_renderD3D9 = BX_NEW(g_allocator, RendererContextD3D9);
		if (!s_renderD3D9->init() )
		{
			BX_DELETE(g_allocator, s_renderD3D9);
			s_renderD3D9 = NULL;
		}
		return s_renderD3D9;
	}

	void rendererDestroy()
	{
		s_renderD3D9->shutdown();
		BX_DELETE(g_allocator, s_renderD3D9);
		s_renderD3D9 = NULL;
	}

	void IndexBufferD3D9::create(uint32_t _size, void* _data, uint16_t _flags)
	{
		m_size    = _size;
		m_flags   = _flags;
		m_dynamic = NULL == _data;

		uint32_t usage = D3DUSAGE_WRITEONLY;
		D3DPOOL  pool  = s_renderD3D9->m_pool;

		if (m_dynamic)
		{
			usage |= D3DUSAGE_DYNAMIC;
			pool = D3DPOOL_DEFAULT;
		}

		const D3DFORMAT format = 0 == (_flags & BGFX_BUFFER_INDEX32)
			? D3DFMT_INDEX16
			: D3DFMT_INDEX32
			;

		DX_CHECK(s_renderD3D9->m_device->CreateIndexBuffer(m_size
			, usage
			, format
			, pool
			, &m_ptr
			, NULL
			) );

		if (NULL != _data)
		{
			update(0, _size, _data);
		}
	}

	void IndexBufferD3D9::preReset()
	{
		if (m_dynamic)
		{
			DX_RELEASE(m_ptr, 0);
		}
	}

	void IndexBufferD3D9::postReset()
	{
		if (m_dynamic)
		{
			const D3DFORMAT format = 0 == (m_flags & BGFX_BUFFER_INDEX32)
				? D3DFMT_INDEX16
				: D3DFMT_INDEX32
				;

			DX_CHECK(s_renderD3D9->m_device->CreateIndexBuffer(m_size
				, D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC
				, format
				, D3DPOOL_DEFAULT
				, &m_ptr
				, NULL
				) );
		}
	}

	void VertexBufferD3D9::create(uint32_t _size, void* _data, VertexDeclHandle _declHandle)
	{
		m_size = _size;
		m_decl = _declHandle;
		m_dynamic = NULL == _data;

		uint32_t usage = D3DUSAGE_WRITEONLY;
		D3DPOOL pool = s_renderD3D9->m_pool;

		if (m_dynamic)
		{
			usage |= D3DUSAGE_DYNAMIC;
			pool = D3DPOOL_DEFAULT;
		}

		DX_CHECK(s_renderD3D9->m_device->CreateVertexBuffer(m_size
				, usage
				, 0
				, pool
				, &m_ptr
				, NULL
				) );

		if (NULL != _data)
		{
			update(0, _size, _data);
		}
	}

	void VertexBufferD3D9::preReset()
	{
		if (m_dynamic)
		{
			DX_RELEASE(m_ptr, 0);
		}
	}

	void VertexBufferD3D9::postReset()
	{
		if (m_dynamic)
		{
			DX_CHECK(s_renderD3D9->m_device->CreateVertexBuffer(m_size
					, D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC
					, 0
					, D3DPOOL_DEFAULT
					, &m_ptr
					, NULL
					) );
		}
	}

	static const D3DVERTEXELEMENT9 s_attrib[] =
	{
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,     0 },
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,       0 },
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,      0 },
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL,     0 },
		{ 0, 0, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,        0 },
		{ 0, 0, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,        1 },
		{ 0, 0, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0 },
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT,  0 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     0 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     1 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     2 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     3 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     4 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     5 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     6 },
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,     7 },
		D3DDECL_END()
	};
	BX_STATIC_ASSERT(Attrib::Count == BX_COUNTOF(s_attrib)-1);

	static const uint8_t s_attribType[][4][2] =
	{
		{ // Uint8
			{ D3DDECLTYPE_UBYTE4,    D3DDECLTYPE_UBYTE4N   },
			{ D3DDECLTYPE_UBYTE4,    D3DDECLTYPE_UBYTE4N   },
			{ D3DDECLTYPE_UBYTE4,    D3DDECLTYPE_UBYTE4N   },
			{ D3DDECLTYPE_UBYTE4,    D3DDECLTYPE_UBYTE4N   },
		},
		{ // Uint10
			{ D3DDECLTYPE_UDEC3,     D3DDECLTYPE_DEC3N     },
			{ D3DDECLTYPE_UDEC3,     D3DDECLTYPE_DEC3N     },
			{ D3DDECLTYPE_UDEC3,     D3DDECLTYPE_DEC3N     },
			{ D3DDECLTYPE_UDEC3,     D3DDECLTYPE_DEC3N     },
		},
		{ // Int16
			{ D3DDECLTYPE_SHORT2,    D3DDECLTYPE_SHORT2N   },
			{ D3DDECLTYPE_SHORT2,    D3DDECLTYPE_SHORT2N   },
			{ D3DDECLTYPE_SHORT4,    D3DDECLTYPE_SHORT4N   },
			{ D3DDECLTYPE_SHORT4,    D3DDECLTYPE_SHORT4N   },
		},
		{ // Half
			{ D3DDECLTYPE_FLOAT16_2, D3DDECLTYPE_FLOAT16_2 },
			{ D3DDECLTYPE_FLOAT16_2, D3DDECLTYPE_FLOAT16_2 },
			{ D3DDECLTYPE_FLOAT16_4, D3DDECLTYPE_FLOAT16_4 },
			{ D3DDECLTYPE_FLOAT16_4, D3DDECLTYPE_FLOAT16_4 },
		},
		{ // Float
			{ D3DDECLTYPE_FLOAT1,    D3DDECLTYPE_FLOAT1    },
			{ D3DDECLTYPE_FLOAT2,    D3DDECLTYPE_FLOAT2    },
			{ D3DDECLTYPE_FLOAT3,    D3DDECLTYPE_FLOAT3    },
			{ D3DDECLTYPE_FLOAT4,    D3DDECLTYPE_FLOAT4    },
		},
	};
	BX_STATIC_ASSERT(AttribType::Count == BX_COUNTOF(s_attribType) );

	static D3DVERTEXELEMENT9* fillVertexDecl(D3DVERTEXELEMENT9* _out, const VertexDecl& _decl)
	{
		D3DVERTEXELEMENT9* elem = _out;

		for (uint32_t attr = 0; attr < Attrib::Count; ++attr)
		{
			if (UINT16_MAX != _decl.m_attributes[attr])
			{
				uint8_t num;
				AttribType::Enum type;
				bool normalized;
				bool asInt;
				_decl.decode(Attrib::Enum(attr), num, type, normalized, asInt);

				memcpy(elem, &s_attrib[attr], sizeof(D3DVERTEXELEMENT9) );

				elem->Type = s_attribType[type][num-1][normalized];
				elem->Offset = _decl.m_offset[attr];
				++elem;
			}
		}

		return elem;
	}

	static IDirect3DVertexDeclaration9* createVertexDeclaration(const VertexDecl& _decl, uint16_t _numInstanceData)
	{
		D3DVERTEXELEMENT9 vertexElements[Attrib::Count+1+BGFX_CONFIG_MAX_INSTANCE_DATA_COUNT];
		D3DVERTEXELEMENT9* elem = fillVertexDecl(vertexElements, _decl);

		const D3DVERTEXELEMENT9 inst = { 1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };

		for (uint8_t ii = 0; ii < _numInstanceData; ++ii)
		{
			memcpy(elem, &inst, sizeof(D3DVERTEXELEMENT9) );
			elem->UsageIndex = uint8_t(7-ii); // TEXCOORD7 = i_data0, TEXCOORD6 = i_data1, etc.
			elem->Offset = ii*16;
			++elem;
		}

		memcpy(elem, &s_attrib[Attrib::Count], sizeof(D3DVERTEXELEMENT9) );

		IDirect3DVertexDeclaration9* ptr;
		DX_CHECK(s_renderD3D9->m_device->CreateVertexDeclaration(vertexElements, &ptr) );
		return ptr;
	}

	void VertexDeclD3D9::create(const VertexDecl& _decl)
	{
		memcpy(&m_decl, &_decl, sizeof(VertexDecl) );
		dump(m_decl);
		m_ptr = createVertexDeclaration(_decl, 0);
	}

	void ShaderD3D9::create(const Memory* _mem)
	{
		bx::MemoryReader reader(_mem->data, _mem->size);

		uint32_t magic;
		bx::read(&reader, magic);

		switch (magic)
		{
		case BGFX_CHUNK_MAGIC_FSH:
		case BGFX_CHUNK_MAGIC_VSH:
			break;

		default:
			BGFX_FATAL(false, Fatal::InvalidShader, "Unknown shader format %x.", magic);
			break;
		}

		bool fragment = BGFX_CHUNK_MAGIC_FSH == magic;

		uint32_t iohash;
		bx::read(&reader, iohash);

		uint16_t count;
		bx::read(&reader, count);

		m_numPredefined = 0;

		BX_TRACE("Shader consts %d", count);

		uint8_t fragmentBit = fragment ? BGFX_UNIFORM_FRAGMENTBIT : 0;

		if (0 < count)
		{
			for (uint32_t ii = 0; ii < count; ++ii)
			{
				uint8_t nameSize;
				bx::read(&reader, nameSize);

				char name[256];
				bx::read(&reader, &name, nameSize);
				name[nameSize] = '\0';

				uint8_t type;
				bx::read(&reader, type);

				uint8_t num;
				bx::read(&reader, num);

				uint16_t regIndex;
				bx::read(&reader, regIndex);

				uint16_t regCount;
				bx::read(&reader, regCount);

				const char* kind = "invalid";

				PredefinedUniform::Enum predefined = nameToPredefinedUniformEnum(name);
				if (PredefinedUniform::Count != predefined)
				{
					kind = "predefined";
					m_predefined[m_numPredefined].m_loc   = regIndex;
					m_predefined[m_numPredefined].m_count = regCount;
					m_predefined[m_numPredefined].m_type  = uint8_t(predefined|fragmentBit);
					m_numPredefined++;
				}
				else
				{
					const UniformInfo* info = s_renderD3D9->m_uniformReg.find(name);
					BX_CHECK(NULL != info, "User defined uniform '%s' is not found, it won't be set.", name);
					if (NULL != info)
					{
						if (NULL == m_constantBuffer)
						{
							m_constantBuffer = ConstantBuffer::create(1024);
						}

						kind = "user";
						m_constantBuffer->writeUniformHandle( (UniformType::Enum)(type|fragmentBit), regIndex, info->m_handle, regCount);
					}
				}

				BX_TRACE("\t%s: %s, type %2d, num %2d, r.index %3d, r.count %2d"
					, kind
					, name
					, type
					, num
					, regIndex
					, regCount
					);
				BX_UNUSED(kind);
			}

			if (NULL != m_constantBuffer)
			{
				m_constantBuffer->finish();
			}
		}

		uint16_t shaderSize;
		bx::read(&reader, shaderSize);

		const DWORD* code = (const DWORD*)reader.getDataPtr();

		if (fragment)
		{
			m_type = 1;
			DX_CHECK(s_renderD3D9->m_device->CreatePixelShader(code, &m_pixelShader) );
			BGFX_FATAL(NULL != m_pixelShader, bgfx::Fatal::InvalidShader, "Failed to create fragment shader.");
		}
		else
		{
			m_type = 0;
			DX_CHECK(s_renderD3D9->m_device->CreateVertexShader(code, &m_vertexShader) );
			BGFX_FATAL(NULL != m_vertexShader, bgfx::Fatal::InvalidShader, "Failed to create vertex shader.");
		}
	}

	void TextureD3D9::createTexture(uint32_t _width, uint32_t _height, uint8_t _numMips)
	{
		m_width = (uint16_t)_width;
		m_height = (uint16_t)_height;
		m_numMips = _numMips;
		m_type = Texture2D;
		const TextureFormat::Enum fmt = (TextureFormat::Enum)m_textureFormat;

		DWORD usage = 0;
		D3DPOOL pool = s_renderD3D9->m_pool;

		const bool renderTarget = 0 != (m_flags&BGFX_TEXTURE_RT_MASK);
		if (isDepth(fmt) )
		{
			usage = D3DUSAGE_DEPTHSTENCIL;
			pool  = D3DPOOL_DEFAULT;
		}
		else if (renderTarget)
		{
			usage = D3DUSAGE_RENDERTARGET;
			pool  = D3DPOOL_DEFAULT;
		}

		if (renderTarget)
		{
			uint32_t msaaQuality = ( (m_flags&BGFX_TEXTURE_RT_MSAA_MASK)>>BGFX_TEXTURE_RT_MSAA_SHIFT);
			msaaQuality = bx::uint32_satsub(msaaQuality, 1);

			bool bufferOnly = 0 != (m_flags&BGFX_TEXTURE_RT_BUFFER_ONLY);

			if (0 != msaaQuality
			||  bufferOnly)
			{
				const Msaa& msaa = s_msaa[msaaQuality];

				if (isDepth(fmt) )
				{
					DX_CHECK(s_renderD3D9->m_device->CreateDepthStencilSurface(
						  m_width
						, m_height
						, s_textureFormat[m_textureFormat].m_fmt
						, msaa.m_type
						, msaa.m_quality
						, FALSE
						, &m_surface
						, NULL
						) );
				}
				else
				{
					DX_CHECK(s_renderD3D9->m_device->CreateRenderTarget(
						  m_width
						, m_height
						, s_textureFormat[m_textureFormat].m_fmt
						, msaa.m_type
						, msaa.m_quality
						, FALSE
						, &m_surface
						, NULL
						) );
				}

				if (bufferOnly)
				{
					// This is render buffer, there is no sampling, no need
					// to create texture.
					return;
				}
			}
		}

		DX_CHECK(s_renderD3D9->m_device->CreateTexture(_width
			, _height
			, _numMips
			, usage
			, s_textureFormat[fmt].m_fmt
			, pool
			, &m_texture2d
			, NULL
			) );

		BGFX_FATAL(NULL != m_texture2d, Fatal::UnableToCreateTexture, "Failed to create texture (size: %dx%d, mips: %d, fmt: %d)."
			, _width
			, _height
			, _numMips
			, getName(fmt)
			);
	}

	void TextureD3D9::createVolumeTexture(uint32_t _width, uint32_t _height, uint32_t _depth, uint32_t _numMips)
	{
		m_type = Texture3D;
		const TextureFormat::Enum fmt = (TextureFormat::Enum)m_textureFormat;

		DX_CHECK(s_renderD3D9->m_device->CreateVolumeTexture(_width
			, _height
			, _depth
			, _numMips
			, 0
			, s_textureFormat[fmt].m_fmt
			, s_renderD3D9->m_pool
			, &m_texture3d
			, NULL
			) );

		BGFX_FATAL(NULL != m_texture3d, Fatal::UnableToCreateTexture, "Failed to create volume texture (size: %dx%dx%d, mips: %d, fmt: %s)."
			, _width
			, _height
			, _depth
			, _numMips
			, getName(fmt)
			);
	}

	void TextureD3D9::createCubeTexture(uint32_t _edge, uint32_t _numMips)
	{
		m_type = TextureCube;
		const TextureFormat::Enum fmt = (TextureFormat::Enum)m_textureFormat;

		DX_CHECK(s_renderD3D9->m_device->CreateCubeTexture(_edge
			, _numMips
			, 0
			, s_textureFormat[fmt].m_fmt
			, s_renderD3D9->m_pool
			, &m_textureCube
			, NULL
			) );

		BGFX_FATAL(NULL != m_textureCube, Fatal::UnableToCreateTexture, "Failed to create cube texture (edge: %d, mips: %d, fmt: %s)."
			, _edge
			, _numMips
			, getName(fmt)
			);
	}

	uint8_t* TextureD3D9::lock(uint8_t _side, uint8_t _lod, uint32_t& _pitch, uint32_t& _slicePitch, const Rect* _rect)
	{
		switch (m_type)
		{
		case Texture2D:
			{
				D3DLOCKED_RECT lockedRect;

				if (NULL != _rect)
				{
					RECT rect;
					rect.left   = _rect->m_x;
					rect.top    = _rect->m_y;
					rect.right  = rect.left + _rect->m_width;
					rect.bottom = rect.top  + _rect->m_height;
					DX_CHECK(m_texture2d->LockRect(_lod, &lockedRect, &rect, 0) );
				}
				else
				{
					DX_CHECK(m_texture2d->LockRect(_lod, &lockedRect, NULL, 0) );
				}

				_pitch = lockedRect.Pitch;
				_slicePitch = 0;
				return (uint8_t*)lockedRect.pBits;
			}

		case Texture3D:
			{
				D3DLOCKED_BOX box;
				DX_CHECK(m_texture3d->LockBox(_lod, &box, NULL, 0) );
				_pitch = box.RowPitch;
				_slicePitch = box.SlicePitch;
				return (uint8_t*)box.pBits;
			}

		case TextureCube:
			{
				D3DLOCKED_RECT lockedRect;

				if (NULL != _rect)
				{
					RECT rect;
					rect.left = _rect->m_x;
					rect.top = _rect->m_y;
					rect.right = rect.left + _rect->m_width;
					rect.bottom = rect.top + _rect->m_height;
					DX_CHECK(m_textureCube->LockRect(D3DCUBEMAP_FACES(_side), _lod, &lockedRect, &rect, 0) );
				}
				else
				{
					DX_CHECK(m_textureCube->LockRect(D3DCUBEMAP_FACES(_side), _lod, &lockedRect, NULL, 0) );
				}

				_pitch = lockedRect.Pitch;
				_slicePitch = 0;
				return (uint8_t*)lockedRect.pBits;
			}
		}

		BX_CHECK(false, "You should not be here.");
		_pitch = 0;
		_slicePitch = 0;
		return NULL;
	}

	void TextureD3D9::unlock(uint8_t _side, uint8_t _lod)
	{
		switch (m_type)
		{
		case Texture2D:
			{
				DX_CHECK(m_texture2d->UnlockRect(_lod) );
			}
			return;

		case Texture3D:
			{
				DX_CHECK(m_texture3d->UnlockBox(_lod) );
			}
			return;

		case TextureCube:
			{
				DX_CHECK(m_textureCube->UnlockRect(D3DCUBEMAP_FACES(_side), _lod) );
			}
			return;
		}

		BX_CHECK(false, "You should not be here.");
	}

	void TextureD3D9::dirty(uint8_t _side, const Rect& _rect, uint16_t _z, uint16_t _depth)
	{
		switch (m_type)
		{
		case Texture2D:
			{
				RECT rect;
				rect.left = _rect.m_x;
				rect.top = _rect.m_y;
				rect.right = rect.left + _rect.m_width;
				rect.bottom = rect.top + _rect.m_height;
				DX_CHECK(m_texture2d->AddDirtyRect(&rect) );
			}
			return;

		case Texture3D:
			{
				D3DBOX box;
				box.Left = _rect.m_x;
				box.Top = _rect.m_y;
				box.Right = box.Left + _rect.m_width;
				box.Bottom = box.Top + _rect.m_height;
				box.Front = _z;
				box.Back = box.Front + _depth;
				DX_CHECK(m_texture3d->AddDirtyBox(&box) );
			}
			return;

		case TextureCube:
			{
				RECT rect;
				rect.left = _rect.m_x;
				rect.top = _rect.m_y;
				rect.right = rect.left + _rect.m_width;
				rect.bottom = rect.top + _rect.m_height;
				DX_CHECK(m_textureCube->AddDirtyRect(D3DCUBEMAP_FACES(_side), &rect) );
			}
			return;
		}

		BX_CHECK(false, "You should not be here.");
	}

	void TextureD3D9::create(const Memory* _mem, uint32_t _flags, uint8_t _skip)
	{
		m_flags = _flags;

		ImageContainer imageContainer;

		if (imageParse(imageContainer, _mem->data, _mem->size) )
		{
			uint8_t numMips = imageContainer.m_numMips;
			const uint8_t startLod = uint8_t(bx::uint32_min(_skip, numMips-1) );
			numMips -= startLod;
			const ImageBlockInfo& blockInfo = getBlockInfo(TextureFormat::Enum(imageContainer.m_format) );
			const uint32_t textureWidth  = bx::uint32_max(blockInfo.blockWidth,  imageContainer.m_width >>startLod);
			const uint32_t textureHeight = bx::uint32_max(blockInfo.blockHeight, imageContainer.m_height>>startLod);

			m_requestedFormat = imageContainer.m_format;
			m_textureFormat   = imageContainer.m_format;

			const TextureFormatInfo& tfi = s_textureFormat[m_requestedFormat];
			uint8_t bpp = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
			if (D3DFMT_UNKNOWN == tfi.m_fmt)
			{
				m_textureFormat = (uint8_t)TextureFormat::BGRA8;
				bpp = 32;
			}

			if (imageContainer.m_cubeMap)
			{
				createCubeTexture(textureWidth, numMips);
			}
			else if (imageContainer.m_depth > 1)
			{
				createVolumeTexture(textureWidth, textureHeight, imageContainer.m_depth, numMips);
			}
			else
			{
				createTexture(textureWidth, textureHeight, numMips);
			}

			if (imageContainer.m_srgb)
			{
				m_flags |= BGFX_TEXTURE_SRGB;
			}

			BX_TRACE("Texture %3d: %s (requested: %s), %dx%d%s%s."
				, this - s_renderD3D9->m_textures
				, getName( (TextureFormat::Enum)m_textureFormat)
				, getName( (TextureFormat::Enum)m_requestedFormat)
				, textureWidth
				, textureHeight
				, imageContainer.m_cubeMap ? "x6" : ""
				, 0 != (m_flags&BGFX_TEXTURE_RT_MASK) ? " (render target)" : ""
				);

			if (0 != (_flags&BGFX_TEXTURE_RT_BUFFER_ONLY) )
			{
				return;
			}

			// For BC4 and B5 in DX9 LockRect returns wrong number of
			// bytes. If actual mip size is used it causes memory corruption.
			// http://www.aras-p.info/texts/D3D9GPUHacks.html#3dc
			const bool useMipSize = true
							&& imageContainer.m_format != TextureFormat::BC4
							&& imageContainer.m_format != TextureFormat::BC5
							;

			const bool convert = m_textureFormat != m_requestedFormat;

			for (uint8_t side = 0, numSides = imageContainer.m_cubeMap ? 6 : 1; side < numSides; ++side)
			{
				uint32_t width     = textureWidth;
				uint32_t height    = textureHeight;
				uint32_t depth     = imageContainer.m_depth;
				uint32_t mipWidth  = imageContainer.m_width;
				uint32_t mipHeight = imageContainer.m_height;

				for (uint8_t lod = 0, num = numMips; lod < num; ++lod)
				{
					width     = bx::uint32_max(1, width);
					height    = bx::uint32_max(1, height);
					depth     = bx::uint32_max(1, depth);
					mipWidth  = bx::uint32_max(blockInfo.blockWidth,  mipWidth);
					mipHeight = bx::uint32_max(blockInfo.blockHeight, mipHeight);
					uint32_t mipSize = width*height*depth*bpp/8;

					ImageMip mip;
					if (imageGetRawData(imageContainer, side, lod+startLod, _mem->data, _mem->size, mip) )
					{
						uint32_t pitch;
						uint32_t slicePitch;
						uint8_t* bits = lock(side, lod, pitch, slicePitch);

						if (convert)
						{
							if (width  != mipWidth
							||  height != mipHeight)
							{
								uint32_t srcpitch = mipWidth*bpp/8;

								uint8_t* temp = (uint8_t*)BX_ALLOC(g_allocator, srcpitch*mipHeight);
								imageDecodeToBgra8(temp
										, mip.m_data
										, mip.m_width
										, mip.m_height
										, srcpitch
										, mip.m_format
										);

								uint32_t dstpitch = pitch;
								for (uint32_t yy = 0; yy < height; ++yy)
								{
									uint8_t* src = &temp[yy*srcpitch];
									uint8_t* dst = &bits[yy*dstpitch];
									memcpy(dst, src, dstpitch);
								}

								BX_FREE(g_allocator, temp);
							}
							else
							{
								imageDecodeToBgra8(bits, mip.m_data, mip.m_width, mip.m_height, pitch, mip.m_format);
							}
						}
						else
						{
							uint32_t size = useMipSize ? mip.m_size : mipSize;
							memcpy(bits, mip.m_data, size);
						}

						unlock(side, lod);
					}

					width     >>= 1;
					height    >>= 1;
					depth     >>= 1;
					mipWidth  >>= 1;
					mipHeight >>= 1;
				}
			}
		}
	}

	void TextureD3D9::updateBegin(uint8_t _side, uint8_t _mip)
	{
		uint32_t slicePitch;
		s_renderD3D9->m_updateTextureSide = _side;
		s_renderD3D9->m_updateTextureMip = _mip;
		s_renderD3D9->m_updateTextureBits = lock(_side, _mip, s_renderD3D9->m_updateTexturePitch, slicePitch);
	}

	void TextureD3D9::update(uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem)
	{
		const uint32_t bpp = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
		const uint32_t rectpitch = _rect.m_width*bpp/8;
		const uint32_t srcpitch  = UINT16_MAX == _pitch ? rectpitch : _pitch;
		const uint32_t dstpitch  = s_renderD3D9->m_updateTexturePitch;
		uint8_t* bits = s_renderD3D9->m_updateTextureBits + _rect.m_y*dstpitch + _rect.m_x*bpp/8;

		const bool convert = m_textureFormat != m_requestedFormat;

		uint8_t* data = _mem->data;
		uint8_t* temp = NULL;

		if (convert)
		{
			temp = (uint8_t*)BX_ALLOC(g_allocator, rectpitch*_rect.m_height);
			imageDecodeToBgra8(temp, data, _rect.m_width, _rect.m_height, srcpitch, m_requestedFormat);
			data = temp;
		}

		{
			uint8_t* src = data;
			uint8_t* dst = bits;
			for (uint32_t yy = 0, height = _rect.m_height; yy < height; ++yy)
			{
				memcpy(dst, src, rectpitch);
				src += srcpitch;
				dst += dstpitch;
			}
		}

		if (NULL != temp)
		{
			BX_FREE(g_allocator, temp);
		}

		if (0 == _mip)
		{
			dirty(_side, _rect, _z, _depth);
		}
	}

	void TextureD3D9::updateEnd()
	{
		unlock(s_renderD3D9->m_updateTextureSide, s_renderD3D9->m_updateTextureMip);
	}

	void TextureD3D9::commit(uint8_t _stage, uint32_t _flags)
	{
		s_renderD3D9->setSamplerState(_stage
			, 0 == (BGFX_SAMPLER_DEFAULT_FLAGS & _flags) ? _flags : m_flags
			);
		IDirect3DDevice9* device = s_renderD3D9->m_device;
		DX_CHECK(device->SetTexture(                           _stage, m_ptr) );
 		DX_CHECK(device->SetTexture(D3DVERTEXTEXTURESAMPLER0 + _stage, m_ptr) );
	}

	void TextureD3D9::resolve() const
	{
		if (NULL != m_surface
		&&  NULL != m_texture2d)
		{
			IDirect3DSurface9* surface;
			DX_CHECK(m_texture2d->GetSurfaceLevel(0, &surface) );
			DX_CHECK(s_renderD3D9->m_device->StretchRect(m_surface
				, NULL
				, surface
				, NULL
				, D3DTEXF_LINEAR
				) );
			DX_RELEASE(surface, 1);
		}
	}

	void TextureD3D9::preReset()
	{
		TextureFormat::Enum fmt = (TextureFormat::Enum)m_textureFormat;
		if (TextureFormat::Unknown != fmt
		&& (isDepth(fmt) || !!(m_flags&BGFX_TEXTURE_RT_MASK) ) )
		{
			DX_RELEASE(m_ptr, 0);
			DX_RELEASE(m_surface, 0);
		}
	}

	void TextureD3D9::postReset()
	{
		TextureFormat::Enum fmt = (TextureFormat::Enum)m_textureFormat;
		if (TextureFormat::Unknown != fmt
		&& (isDepth(fmt) || !!(m_flags&BGFX_TEXTURE_RT_MASK) ) )
		{
			createTexture(m_width, m_height, m_numMips);
		}
	}

	void FrameBufferD3D9::create(uint8_t _num, const TextureHandle* _handles)
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_color); ++ii)
		{
			m_color[ii] = NULL;
		}
		m_depthStencil = NULL;

		m_num = 0;
		m_needResolve = false;
		for (uint32_t ii = 0; ii < _num; ++ii)
		{
			TextureHandle handle = _handles[ii];
			if (isValid(handle) )
			{
				const TextureD3D9& texture = s_renderD3D9->m_textures[handle.idx];

				if (isDepth( (TextureFormat::Enum)texture.m_textureFormat) )
				{
					m_depthHandle = handle;
					if (NULL != texture.m_surface)
					{
						m_depthStencil = texture.m_surface;
						m_depthStencil->AddRef();
					}
					else
					{
						DX_CHECK(texture.m_texture2d->GetSurfaceLevel(0, &m_depthStencil) );
					}
				}
				else
				{
					m_colorHandle[m_num] = handle;
					if (NULL != texture.m_surface)
					{
						m_color[m_num] = texture.m_surface;
						m_color[m_num]->AddRef();
					}
					else
					{
						DX_CHECK(texture.m_texture2d->GetSurfaceLevel(0, &m_color[m_num]) );
					}
					m_num++;
				}

				m_needResolve |= (NULL != texture.m_surface) && (NULL != texture.m_texture2d);
			}
		}

		if (0 == m_num)
		{
			createNullColorRT();
		}
	}

	void FrameBufferD3D9::create(uint16_t _denseIdx, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat)
	{
		BX_UNUSED(_depthFormat);

		m_hwnd = (HWND)_nwh;

		m_width  = bx::uint32_max(_width,  16);
		m_height = bx::uint32_max(_height, 16);

		D3DPRESENT_PARAMETERS params;
		memcpy(&params, &s_renderD3D9->m_params, sizeof(D3DPRESENT_PARAMETERS) );
		params.BackBufferWidth  = m_width;
		params.BackBufferHeight = m_height;

		DX_CHECK(s_renderD3D9->m_device->CreateAdditionalSwapChain(&params, &m_swapChain) );
		DX_CHECK(m_swapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &m_color[0]) );

		DX_CHECK(s_renderD3D9->m_device->CreateDepthStencilSurface(
			  params.BackBufferWidth
			, params.BackBufferHeight
			, params.AutoDepthStencilFormat
			, params.MultiSampleType
			, params.MultiSampleQuality
			, FALSE
			, &m_depthStencil
			, NULL
			) );

		m_colorHandle[0].idx = invalidHandle;
		m_denseIdx = _denseIdx;
		m_num = 1;
		m_needResolve = false;
	}

	uint16_t FrameBufferD3D9::destroy()
	{
		if (NULL != m_hwnd)
		{
			DX_RELEASE(m_depthStencil, 0);
			DX_RELEASE(m_color[0],     0);
			DX_RELEASE(m_swapChain,    0);
		}
		else
		{
			for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
			{
				m_colorHandle[ii].idx = invalidHandle;

				IDirect3DSurface9* ptr = m_color[ii];
				if (NULL != ptr)
				{
					ptr->Release();
					m_color[ii] = NULL;
				}
			}

			if (NULL != m_depthStencil)
			{
				if (0 == m_num)
				{
					IDirect3DSurface9* ptr = m_color[0];
					if (NULL != ptr)
					{
						ptr->Release();
						m_color[0] = NULL;
					}
				}

				m_depthStencil->Release();
				m_depthStencil = NULL;
			}
		}

		m_hwnd = NULL;
		m_num = 0;
		m_depthHandle.idx = invalidHandle;

		uint16_t denseIdx = m_denseIdx;
		m_denseIdx = UINT16_MAX;

		return denseIdx;
	}

	HRESULT FrameBufferD3D9::present()
	{
		return m_swapChain->Present(NULL, NULL, m_hwnd, NULL, 0);
	}

	void FrameBufferD3D9::resolve() const
	{
		if (m_needResolve)
		{
			if (isValid(m_depthHandle) )
			{
				const TextureD3D9& texture = s_renderD3D9->m_textures[m_depthHandle.idx];
				texture.resolve();
			}

			for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
			{
				const TextureD3D9& texture = s_renderD3D9->m_textures[m_colorHandle[ii].idx];
				texture.resolve();
			}
		}
	}

	void FrameBufferD3D9::preReset()
	{
		if (NULL != m_hwnd)
		{
			DX_RELEASE(m_color[0], 0);
			DX_RELEASE(m_depthStencil, 0);
			DX_RELEASE(m_swapChain, 0);
		}
		else
		{
			for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
			{
				m_color[ii]->Release();
				m_color[ii] = NULL;
			}

			if (isValid(m_depthHandle) )
			{
				if (0 == m_num)
				{
					m_color[0]->Release();
					m_color[0] = NULL;
				}

				m_depthStencil->Release();
				m_depthStencil = NULL;
			}
		}
	}

	void FrameBufferD3D9::postReset()
	{
		if (NULL != m_hwnd)
		{
			D3DPRESENT_PARAMETERS params;
			memcpy(&params, &s_renderD3D9->m_params, sizeof(D3DPRESENT_PARAMETERS) );
			params.BackBufferWidth  = m_width;
			params.BackBufferHeight = m_height;

			DX_CHECK(s_renderD3D9->m_device->CreateAdditionalSwapChain(&params, &m_swapChain) );
			DX_CHECK(m_swapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &m_color[0]) );
			DX_CHECK(s_renderD3D9->m_device->CreateDepthStencilSurface(params.BackBufferWidth
					, params.BackBufferHeight
					, params.AutoDepthStencilFormat
					, params.MultiSampleType
					, params.MultiSampleQuality
					, FALSE
					, &m_depthStencil
					, NULL
					) );
		}
		else
		{
			for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
			{
				TextureHandle th = m_colorHandle[ii];

				if (isValid(th) )
				{
					TextureD3D9& texture = s_renderD3D9->m_textures[th.idx];
					if (NULL != texture.m_surface)
					{
						m_color[ii] = texture.m_surface;
						m_color[ii]->AddRef();
					}
					else
					{
						DX_CHECK(texture.m_texture2d->GetSurfaceLevel(0, &m_color[ii]) );
					}
				}
			}

			if (isValid(m_depthHandle) )
			{
				TextureD3D9& texture = s_renderD3D9->m_textures[m_depthHandle.idx];
				if (NULL != texture.m_surface)
				{
					m_depthStencil = texture.m_surface;
					m_depthStencil->AddRef();
				}
				else
				{
					DX_CHECK(texture.m_texture2d->GetSurfaceLevel(0, &m_depthStencil) );
				}

				if (0 == m_num)
				{
					createNullColorRT();
				}
			}
		}
	}

	void FrameBufferD3D9::createNullColorRT()
	{
		const TextureD3D9& texture = s_renderD3D9->m_textures[m_depthHandle.idx];
		DX_CHECK(s_renderD3D9->m_device->CreateRenderTarget(texture.m_width
			, texture.m_height
			, D3DFMT_NULL
			, D3DMULTISAMPLE_NONE
			, 0
			, false
			, &m_color[0]
			, NULL
			) );
	}

	void TimerQueryD3D9::postReset()
	{
		IDirect3DDevice9* device = s_renderD3D9->m_device;

		for (uint32_t ii = 0; ii < BX_COUNTOF(m_frame); ++ii)
		{
			Frame& frame = m_frame[ii];
			DX_CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &frame.m_disjoint) );
			DX_CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,         &frame.m_start) );
			DX_CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,         &frame.m_end) );
			DX_CHECK(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,     &frame.m_freq) );
		}

		m_elapsed   = 0;
		m_frequency = 1;
		m_control.reset();
	}

	void TimerQueryD3D9::preReset()
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(m_frame); ++ii)
		{
			Frame& frame = m_frame[ii];
			DX_RELEASE(frame.m_disjoint, 0);
			DX_RELEASE(frame.m_start, 0);
			DX_RELEASE(frame.m_end, 0);
			DX_RELEASE(frame.m_freq, 0);
		}
	}

	void TimerQueryD3D9::begin()
	{
		while (0 == m_control.reserve(1) )
		{
			get();
		}

		Frame& frame = m_frame[m_control.m_current];
		frame.m_disjoint->Issue(D3DISSUE_BEGIN);
		frame.m_start->Issue(D3DISSUE_END);
	}

	void TimerQueryD3D9::end()
	{
		Frame& frame = m_frame[m_control.m_current];
		frame.m_disjoint->Issue(D3DISSUE_END);
		frame.m_end->Issue(D3DISSUE_END);
		frame.m_freq->Issue(D3DISSUE_END);
		m_control.commit(1);
	}

	bool TimerQueryD3D9::get()
	{
		if (0 != m_control.available() )
		{
			Frame& frame = m_frame[m_control.m_read];

			uint64_t freq;
			HRESULT hr = frame.m_freq->GetData(&freq, sizeof(freq), 0);
			if (S_OK == hr)
			{
				m_control.consume(1);

				uint64_t timeStart;
				DX_CHECK(frame.m_start->GetData(&timeStart, sizeof(timeStart), 0) );

				uint64_t timeEnd;
				DX_CHECK(frame.m_end->GetData(&timeEnd, sizeof(timeEnd), 0) );

				m_frequency = freq;
				m_elapsed   = timeEnd - timeStart;

				return true;
			}
		}

		return false;
	}

	void RendererContextD3D9::submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter)
	{
		IDirect3DDevice9* device = m_device;

		PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), L"rendererSubmit");

		updateResolution(_render->m_resolution);

		int64_t elapsed = -bx::getHPCounter();
		int64_t captureElapsed = 0;

		device->BeginScene();
		if (m_timerQuerySupport)
		{
			m_gpuTimer.begin();
		}

		if (0 < _render->m_iboffset)
		{
			TransientIndexBuffer* ib = _render->m_transientIb;
			m_indexBuffers[ib->handle.idx].update(0, _render->m_iboffset, ib->data, true);
		}

		if (0 < _render->m_vboffset)
		{
			TransientVertexBuffer* vb = _render->m_transientVb;
			m_vertexBuffers[vb->handle.idx].update(0, _render->m_vboffset, vb->data, true);
		}

		_render->sort();

		RenderDraw currentState;
		currentState.clear();
		currentState.m_flags = BGFX_STATE_NONE;
		currentState.m_stencil = packStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);

		ViewState viewState(_render, false);

		DX_CHECK(device->SetRenderState(D3DRS_FILLMODE, _render->m_debug&BGFX_DEBUG_WIREFRAME ? D3DFILL_WIREFRAME : D3DFILL_SOLID) );
		uint16_t programIdx = invalidHandle;
		SortKey key;
		uint16_t view = UINT16_MAX;
		FrameBufferHandle fbh = BGFX_INVALID_HANDLE;
		uint32_t blendFactor = 0;

		uint8_t primIndex;
		{
			const uint64_t pt = _render->m_debug&BGFX_DEBUG_WIREFRAME ? BGFX_STATE_PT_LINES : 0;
			primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
		}
		PrimInfo prim = s_primInfo[primIndex];

		bool viewHasScissor = false;
		Rect viewScissorRect;
		viewScissorRect.clear();

		uint32_t statsNumPrimsSubmitted[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumPrimsRendered[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumInstances[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumIndices = 0;
		uint32_t statsKeyType[2] = {};

		invalidateSamplerState();

		if (0 == (_render->m_debug&BGFX_DEBUG_IFH) )
		{
			for (uint32_t item = 0, numItems = _render->m_num; item < numItems; ++item)
			{
				const bool isCompute = key.decode(_render->m_sortKeys[item], _render->m_viewRemap);
				statsKeyType[isCompute]++;

				if (isCompute)
				{
					BX_CHECK(false, "Compute is not supported on DirectX 9.");
					continue;
				}

				const RenderDraw& draw = _render->m_renderItem[_render->m_sortValues[item] ].draw;

				const uint64_t newFlags = draw.m_flags;
				uint64_t changedFlags = currentState.m_flags ^ draw.m_flags;
				currentState.m_flags = newFlags;

				const uint64_t newStencil = draw.m_stencil;
				uint64_t changedStencil = currentState.m_stencil ^ draw.m_stencil;
				currentState.m_stencil = newStencil;

				if (key.m_view != view)
				{
					currentState.clear();
					currentState.m_scissor = !draw.m_scissor;
					changedFlags = BGFX_STATE_MASK;
					changedStencil = packStencil(BGFX_STENCIL_MASK, BGFX_STENCIL_MASK);
					currentState.m_flags = newFlags;
					currentState.m_stencil = newStencil;

					PIX_ENDEVENT();
					PIX_BEGINEVENT(D3DCOLOR_RGBA(0xff, 0x00, 0x00, 0xff), s_viewNameW[key.m_view]);

					view = key.m_view;
					programIdx = invalidHandle;

					if (_render->m_fb[view].idx != fbh.idx)
					{
						fbh = _render->m_fb[view];
						setFrameBuffer(fbh);
					}

					viewState.m_rect        = _render->m_rect[view];
					const Rect& scissorRect = _render->m_scissor[view];
					viewHasScissor  = !scissorRect.isZero();
					viewScissorRect = viewHasScissor ? scissorRect : viewState.m_rect;

					D3DVIEWPORT9 vp;
					vp.X      = viewState.m_rect.m_x;
					vp.Y      = viewState.m_rect.m_y;
					vp.Width  = viewState.m_rect.m_width;
					vp.Height = viewState.m_rect.m_height;
					vp.MinZ = 0.0f;
					vp.MaxZ = 1.0f;
					DX_CHECK(device->SetViewport(&vp) );

					Clear& clear = _render->m_clear[view];

					if (BGFX_CLEAR_NONE != (clear.m_flags & BGFX_CLEAR_MASK) )
					{
						clearQuad(_clearQuad, viewState.m_rect, clear, _render->m_clearColor);
						prim = s_primInfo[BX_COUNTOF(s_primName)]; // Force primitive type update after clear quad.
					}

					DX_CHECK(device->SetRenderState(D3DRS_STENCILENABLE, FALSE) );
					DX_CHECK(device->SetRenderState(D3DRS_ZENABLE, TRUE) );
					DX_CHECK(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS) );
					DX_CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE) );
					DX_CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE) );
					DX_CHECK(device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER) );
				}

				uint16_t scissor = draw.m_scissor;
				if (currentState.m_scissor != scissor)
				{
					currentState.m_scissor = scissor;

					if (UINT16_MAX == scissor)
					{
						DX_CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, viewHasScissor) );
						if (viewHasScissor)
						{
							RECT rc;
							rc.left   = viewScissorRect.m_x;
							rc.top    = viewScissorRect.m_y;
							rc.right  = viewScissorRect.m_x + viewScissorRect.m_width;
							rc.bottom = viewScissorRect.m_y + viewScissorRect.m_height;
							DX_CHECK(device->SetScissorRect(&rc) );
						}
					}
					else
					{
						Rect scissorRect;
						scissorRect.intersect(viewScissorRect, _render->m_rectCache.m_cache[scissor]);
						DX_CHECK(device->SetRenderState(D3DRS_SCISSORTESTENABLE, true) );
						RECT rc;
						rc.left   = scissorRect.m_x;
						rc.top    = scissorRect.m_y;
						rc.right  = scissorRect.m_x + scissorRect.m_width;
						rc.bottom = scissorRect.m_y + scissorRect.m_height;
						DX_CHECK(device->SetScissorRect(&rc) );
					}
				}

				if (0 != changedStencil)
				{
					bool enable = 0 != newStencil;
					DX_CHECK(device->SetRenderState(D3DRS_STENCILENABLE, enable) );

					if (0 != newStencil)
					{
						uint32_t fstencil = unpackStencil(0, newStencil);
						uint32_t bstencil = unpackStencil(1, newStencil);
						uint8_t frontAndBack = bstencil != BGFX_STENCIL_NONE && bstencil != fstencil;
						DX_CHECK(device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, 0 != frontAndBack) );

						uint32_t fchanged = unpackStencil(0, changedStencil);
						if ( (BGFX_STENCIL_FUNC_REF_MASK|BGFX_STENCIL_FUNC_RMASK_MASK) & fchanged)
						{
							uint32_t ref = (fstencil&BGFX_STENCIL_FUNC_REF_MASK)>>BGFX_STENCIL_FUNC_REF_SHIFT;
							DX_CHECK(device->SetRenderState(D3DRS_STENCILREF, ref) );

							uint32_t rmask = (fstencil&BGFX_STENCIL_FUNC_RMASK_MASK)>>BGFX_STENCIL_FUNC_RMASK_SHIFT;
							DX_CHECK(device->SetRenderState(D3DRS_STENCILMASK, rmask) );
						}

// 						uint32_t bchanged = unpackStencil(1, changedStencil);
// 						if (BGFX_STENCIL_FUNC_RMASK_MASK & bchanged)
// 						{
// 							uint32_t wmask = (bstencil&BGFX_STENCIL_FUNC_RMASK_MASK)>>BGFX_STENCIL_FUNC_RMASK_SHIFT;
// 							DX_CHECK(device->SetRenderState(D3DRS_STENCILWRITEMASK, wmask) );
// 						}

						for (uint8_t ii = 0, num = frontAndBack+1; ii < num; ++ii)
						{
							uint32_t stencil = unpackStencil(ii, newStencil);
							uint32_t changed = unpackStencil(ii, changedStencil);

							if ( (BGFX_STENCIL_TEST_MASK|BGFX_STENCIL_FUNC_REF_MASK|BGFX_STENCIL_FUNC_RMASK_MASK) & changed)
							{
								uint32_t func = (stencil&BGFX_STENCIL_TEST_MASK)>>BGFX_STENCIL_TEST_SHIFT;
								DX_CHECK(device->SetRenderState(s_stencilFuncRs[ii], s_cmpFunc[func]) );
							}

							if ( (BGFX_STENCIL_OP_FAIL_S_MASK|BGFX_STENCIL_OP_FAIL_Z_MASK|BGFX_STENCIL_OP_PASS_Z_MASK) & changed)
							{
								uint32_t sfail = (stencil&BGFX_STENCIL_OP_FAIL_S_MASK)>>BGFX_STENCIL_OP_FAIL_S_SHIFT;
								DX_CHECK(device->SetRenderState(s_stencilFailRs[ii], s_stencilOp[sfail]) );

								uint32_t zfail = (stencil&BGFX_STENCIL_OP_FAIL_Z_MASK)>>BGFX_STENCIL_OP_FAIL_Z_SHIFT;
								DX_CHECK(device->SetRenderState(s_stencilZFailRs[ii], s_stencilOp[zfail]) );

								uint32_t zpass = (stencil&BGFX_STENCIL_OP_PASS_Z_MASK)>>BGFX_STENCIL_OP_PASS_Z_SHIFT;
								DX_CHECK(device->SetRenderState(s_stencilZPassRs[ii], s_stencilOp[zpass]) );
							}
						}
					}
				}

				if ( (0
					 | BGFX_STATE_CULL_MASK
					 | BGFX_STATE_DEPTH_WRITE
					 | BGFX_STATE_DEPTH_TEST_MASK
					 | BGFX_STATE_RGB_WRITE
					 | BGFX_STATE_ALPHA_WRITE
					 | BGFX_STATE_BLEND_MASK
					 | BGFX_STATE_BLEND_EQUATION_MASK
					 | BGFX_STATE_ALPHA_REF_MASK
					 | BGFX_STATE_PT_MASK
					 | BGFX_STATE_POINT_SIZE_MASK
					 | BGFX_STATE_MSAA
					 ) & changedFlags)
				{
					if (BGFX_STATE_CULL_MASK & changedFlags)
					{
						uint32_t cull = (newFlags&BGFX_STATE_CULL_MASK)>>BGFX_STATE_CULL_SHIFT;
						DX_CHECK(device->SetRenderState(D3DRS_CULLMODE, s_cullMode[cull]) );
					}

					if (BGFX_STATE_DEPTH_WRITE & changedFlags)
					{
						DX_CHECK(device->SetRenderState(D3DRS_ZWRITEENABLE, !!(BGFX_STATE_DEPTH_WRITE & newFlags) ) );
					}

					if (BGFX_STATE_DEPTH_TEST_MASK & changedFlags)
					{
						uint32_t func = (newFlags&BGFX_STATE_DEPTH_TEST_MASK)>>BGFX_STATE_DEPTH_TEST_SHIFT;
						DX_CHECK(device->SetRenderState(D3DRS_ZENABLE, 0 != func) );

						if (0 != func)
						{
							DX_CHECK(device->SetRenderState(D3DRS_ZFUNC, s_cmpFunc[func]) );
						}
					}

					if (BGFX_STATE_ALPHA_REF_MASK & changedFlags)
					{
						uint32_t ref = (newFlags&BGFX_STATE_ALPHA_REF_MASK)>>BGFX_STATE_ALPHA_REF_SHIFT;
						viewState.m_alphaRef = ref/255.0f;
					}

					if ( (BGFX_STATE_PT_POINTS|BGFX_STATE_POINT_SIZE_MASK) & changedFlags)
					{
						DX_CHECK(device->SetRenderState(D3DRS_POINTSIZE, castfu( (float)( (newFlags&BGFX_STATE_POINT_SIZE_MASK)>>BGFX_STATE_POINT_SIZE_SHIFT) ) ) );
					}

					if (BGFX_STATE_MSAA & changedFlags)
					{
						DX_CHECK(device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, (newFlags&BGFX_STATE_MSAA) == BGFX_STATE_MSAA) );
					}

					if ( (BGFX_STATE_ALPHA_WRITE|BGFX_STATE_RGB_WRITE) & changedFlags)
					{
						uint32_t writeEnable = (newFlags&BGFX_STATE_ALPHA_WRITE) ? D3DCOLORWRITEENABLE_ALPHA : 0;
 						writeEnable |= (newFlags&BGFX_STATE_RGB_WRITE) ? D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE : 0;
						DX_CHECK(device->SetRenderState(D3DRS_COLORWRITEENABLE, writeEnable) );
					}

					if ( (BGFX_STATE_BLEND_MASK|BGFX_STATE_BLEND_EQUATION_MASK) & changedFlags
					||  blendFactor != draw.m_rgba)
					{
						bool enabled = !!(BGFX_STATE_BLEND_MASK & newFlags);
						DX_CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, enabled) );

						if (enabled)
						{
							const uint32_t blend    = uint32_t( (newFlags&BGFX_STATE_BLEND_MASK)>>BGFX_STATE_BLEND_SHIFT);
							const uint32_t equation = uint32_t( (newFlags&BGFX_STATE_BLEND_EQUATION_MASK)>>BGFX_STATE_BLEND_EQUATION_SHIFT);

							const uint32_t srcRGB  = (blend    )&0xf;
							const uint32_t dstRGB  = (blend>> 4)&0xf;
							const uint32_t srcA    = (blend>> 8)&0xf;
							const uint32_t dstA    = (blend>>12)&0xf;

							const uint32_t equRGB = (equation   )&0x7;
							const uint32_t equA   = (equation>>3)&0x7;

 							DX_CHECK(device->SetRenderState(D3DRS_SRCBLEND,  s_blendFactor[srcRGB].m_src) );
							DX_CHECK(device->SetRenderState(D3DRS_DESTBLEND, s_blendFactor[dstRGB].m_dst) );
							DX_CHECK(device->SetRenderState(D3DRS_BLENDOP,   s_blendEquation[equRGB]) );

							const bool separate = srcRGB != srcA || dstRGB != dstA || equRGB != equA;

							DX_CHECK(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, separate) );
							if (separate)
							{
								DX_CHECK(device->SetRenderState(D3DRS_SRCBLENDALPHA,  s_blendFactor[srcA].m_src) );
								DX_CHECK(device->SetRenderState(D3DRS_DESTBLENDALPHA, s_blendFactor[dstA].m_dst) );
								DX_CHECK(device->SetRenderState(D3DRS_BLENDOPALPHA,   s_blendEquation[equA]) );
							}

							if ( (s_blendFactor[srcRGB].m_factor || s_blendFactor[dstRGB].m_factor)
							&&  blendFactor != draw.m_rgba)
							{
								const uint32_t rgba = draw.m_rgba;
								D3DCOLOR color = D3DCOLOR_RGBA(rgba>>24
															, (rgba>>16)&0xff
															, (rgba>> 8)&0xff
															, (rgba    )&0xff
															);
								DX_CHECK(device->SetRenderState(D3DRS_BLENDFACTOR, color) );
							}
						}

						blendFactor = draw.m_rgba;
					}

					const uint64_t pt = _render->m_debug&BGFX_DEBUG_WIREFRAME ? BGFX_STATE_PT_LINES : newFlags&BGFX_STATE_PT_MASK;
					primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
					prim = s_primInfo[primIndex];
				}

				bool programChanged = false;
				bool constantsChanged = draw.m_constBegin < draw.m_constEnd;
				rendererUpdateUniforms(this, _render->m_constantBuffer, draw.m_constBegin, draw.m_constEnd);

				if (key.m_program != programIdx)
				{
					programIdx = key.m_program;

					if (invalidHandle == programIdx)
					{
						device->SetVertexShader(NULL);
						device->SetPixelShader(NULL);
					}
					else
					{
						ProgramD3D9& program = m_program[programIdx];
						device->SetVertexShader(program.m_vsh->m_vertexShader);
						device->SetPixelShader(program.m_fsh->m_pixelShader);
					}

					programChanged =
						constantsChanged = true;
				}

				if (invalidHandle != programIdx)
				{
					ProgramD3D9& program = m_program[programIdx];

					if (constantsChanged)
					{
						ConstantBuffer* vcb = program.m_vsh->m_constantBuffer;
						if (NULL != vcb)
						{
							commit(*vcb);
						}

						ConstantBuffer* fcb = program.m_fsh->m_constantBuffer;
						if (NULL != fcb)
						{
							commit(*fcb);
						}
					}

					viewState.setPredefined<4>(this, view, 0, program, _render, draw);
				}

				{
					for (uint8_t stage = 0; stage < BGFX_CONFIG_MAX_TEXTURE_SAMPLERS; ++stage)
					{
						const Binding& bind = draw.m_bind[stage];
						Binding& current = currentState.m_bind[stage];
						if (current.m_idx != bind.m_idx
						||  current.m_un.m_draw.m_flags != bind.m_un.m_draw.m_flags
						||  programChanged)
						{
							if (invalidHandle != bind.m_idx)
							{
								m_textures[bind.m_idx].commit(stage, bind.m_un.m_draw.m_flags);
							}
							else
							{
								DX_CHECK(device->SetTexture(stage, NULL) );
							}
						}

						current = bind;
					}
				}

				if (programChanged
				||  currentState.m_vertexBuffer.idx != draw.m_vertexBuffer.idx
				||  currentState.m_instanceDataBuffer.idx != draw.m_instanceDataBuffer.idx
				||  currentState.m_instanceDataOffset != draw.m_instanceDataOffset
				||  currentState.m_instanceDataStride != draw.m_instanceDataStride)
				{
					currentState.m_vertexBuffer = draw.m_vertexBuffer;
					currentState.m_instanceDataBuffer.idx = draw.m_instanceDataBuffer.idx;
					currentState.m_instanceDataOffset = draw.m_instanceDataOffset;
					currentState.m_instanceDataStride = draw.m_instanceDataStride;

					uint16_t handle = draw.m_vertexBuffer.idx;
					if (invalidHandle != handle)
					{
						const VertexBufferD3D9& vb = m_vertexBuffers[handle];

						uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
						const VertexDeclD3D9& vertexDecl = m_vertexDecls[decl];
						DX_CHECK(device->SetStreamSource(0, vb.m_ptr, 0, vertexDecl.m_decl.m_stride) );

						if (isValid(draw.m_instanceDataBuffer)
						&&  m_instancingSupport)
						{
							const VertexBufferD3D9& inst = m_vertexBuffers[draw.m_instanceDataBuffer.idx];
							DX_CHECK(device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA|draw.m_numInstances) );
							DX_CHECK(device->SetStreamSourceFreq(1, UINT(D3DSTREAMSOURCE_INSTANCEDATA|1) ) );
							DX_CHECK(device->SetStreamSource(1, inst.m_ptr, draw.m_instanceDataOffset, draw.m_instanceDataStride) );

							IDirect3DVertexDeclaration9* ptr = createVertexDeclaration(vertexDecl.m_decl, draw.m_instanceDataStride/16);
							DX_CHECK(device->SetVertexDeclaration(ptr) );
							DX_RELEASE(ptr, 0);
						}
						else
						{
							DX_CHECK(device->SetStreamSourceFreq(0, 1) );
							DX_CHECK(device->SetStreamSource(1, NULL, 0, 0) );
							DX_CHECK(device->SetVertexDeclaration(vertexDecl.m_ptr) );
						}
					}
					else
					{
						DX_CHECK(device->SetStreamSource(0, NULL, 0, 0) );
						DX_CHECK(device->SetStreamSource(1, NULL, 0, 0) );
					}
				}

				if (currentState.m_indexBuffer.idx != draw.m_indexBuffer.idx)
				{
					currentState.m_indexBuffer = draw.m_indexBuffer;

					uint16_t handle = draw.m_indexBuffer.idx;
					if (invalidHandle != handle)
					{
						const IndexBufferD3D9& ib = m_indexBuffers[handle];
						DX_CHECK(device->SetIndices(ib.m_ptr) );
					}
					else
					{
						DX_CHECK(device->SetIndices(NULL) );
					}
				}

				if (isValid(currentState.m_vertexBuffer) )
				{
					uint32_t numVertices = draw.m_numVertices;
					if (UINT32_MAX == numVertices)
					{
						const VertexBufferD3D9& vb = m_vertexBuffers[currentState.m_vertexBuffer.idx];
						uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
						const VertexDeclD3D9& vertexDecl = m_vertexDecls[decl];
						numVertices = vb.m_size/vertexDecl.m_decl.m_stride;
					}

					uint32_t numIndices = 0;
					uint32_t numPrimsSubmitted = 0;
					uint32_t numInstances = 0;
					uint32_t numPrimsRendered = 0;

					if (isValid(draw.m_indexBuffer) )
					{
						if (UINT32_MAX == draw.m_numIndices)
						{
							const IndexBufferD3D9& ib = m_indexBuffers[draw.m_indexBuffer.idx];
							const uint32_t indexSize = 0 == (ib.m_flags & BGFX_BUFFER_INDEX32) ? 2 : 4;
							numIndices        = ib.m_size/indexSize;
							numPrimsSubmitted = numIndices/prim.m_div - prim.m_sub;
							numInstances      = draw.m_numInstances;
							numPrimsRendered  = numPrimsSubmitted*draw.m_numInstances;

							DX_CHECK(device->DrawIndexedPrimitive(prim.m_type
								, draw.m_startVertex
								, 0
								, numVertices
								, 0
								, numPrimsSubmitted
								) );
						}
						else if (prim.m_min <= draw.m_numIndices)
						{
							numIndices = draw.m_numIndices;
							numPrimsSubmitted = numIndices/prim.m_div - prim.m_sub;
							numInstances = draw.m_numInstances;
							numPrimsRendered = numPrimsSubmitted*draw.m_numInstances;

							DX_CHECK(device->DrawIndexedPrimitive(prim.m_type
								, draw.m_startVertex
								, 0
								, numVertices
								, draw.m_startIndex
								, numPrimsSubmitted
								) );
						}
					}
					else
					{
						numPrimsSubmitted = numVertices/prim.m_div - prim.m_sub;
						numInstances = draw.m_numInstances;
						numPrimsRendered = numPrimsSubmitted*draw.m_numInstances;

						DX_CHECK(device->DrawPrimitive(prim.m_type
							, draw.m_startVertex
							, numPrimsSubmitted
							) );
					}

					statsNumPrimsSubmitted[primIndex] += numPrimsSubmitted;
					statsNumPrimsRendered[primIndex]  += numPrimsRendered;
					statsNumInstances[primIndex]      += numInstances;
					statsNumIndices += numIndices;
				}
			}

			if (0 < _render->m_num)
			{
				if (0 != (m_resolution.m_flags & BGFX_RESET_FLUSH_AFTER_RENDER) )
				{
					m_flushQuery->Issue(D3DISSUE_END);
					m_flushQuery->GetData(NULL, 0, D3DGETDATA_FLUSH);
				}

				captureElapsed = -bx::getHPCounter();
				capture();
				captureElapsed += bx::getHPCounter();
			}
		}

		PIX_ENDEVENT();

		int64_t now = bx::getHPCounter();
		elapsed += now;

		static int64_t last = now;
		int64_t frameTime = now - last;
		last = now;

		static int64_t min = frameTime;
		static int64_t max = frameTime;
		min = min > frameTime ? frameTime : min;
		max = max < frameTime ? frameTime : max;

		static uint32_t maxGpuLatency = 0;
		static double   maxGpuElapsed = 0.0f;
		double elapsedGpuMs = 0.0;

		if (m_timerQuerySupport)
		{
			m_gpuTimer.end();

			while (m_gpuTimer.get() )
			{
				double toGpuMs = 1000.0 / double(m_gpuTimer.m_frequency);
				elapsedGpuMs   = m_gpuTimer.m_elapsed * toGpuMs;
				maxGpuElapsed  = elapsedGpuMs > maxGpuElapsed ? elapsedGpuMs : maxGpuElapsed;
			}
			maxGpuLatency = bx::uint32_imax(maxGpuLatency, m_gpuTimer.m_control.available()-1);
		}

		const int64_t timerFreq = bx::getHPFrequency();

		Stats& perfStats   = _render->m_perfStats;
		perfStats.cpuTime      = frameTime;
		perfStats.cpuTimerFreq = timerFreq;
		perfStats.gpuTime      = m_gpuTimer.m_elapsed;
		perfStats.gpuTimerFreq = m_gpuTimer.m_frequency;

		if (_render->m_debug & (BGFX_DEBUG_IFH|BGFX_DEBUG_STATS) )
		{
			PIX_BEGINEVENT(D3DCOLOR_RGBA(0x40, 0x40, 0x40, 0xff), L"debugstats");

			TextVideoMem& tvm = m_textVideoMem;

			static int64_t next = now;

			if (now >= next)
			{
				next = now + timerFreq;

				double freq = double(timerFreq);
				double toMs = 1000.0/freq;

				tvm.clear();
				uint16_t pos = 0;
				tvm.printf(0, pos++, BGFX_CONFIG_DEBUG ? 0x89 : 0x8f, " %s / " BX_COMPILER_NAME " / " BX_CPU_NAME " / " BX_ARCH_NAME " / " BX_PLATFORM_NAME " "
					, getRendererName()
					);

				const D3DADAPTER_IDENTIFIER9& identifier = m_identifier;
				tvm.printf(0, pos++, 0x8f, " Device: %s (%s)", identifier.Description, identifier.Driver);

				pos = 10;
				tvm.printf(10, pos++, 0x8e, "       Frame: %7.3f, % 7.3f \x1f, % 7.3f \x1e [ms] / % 6.2f FPS "
					, double(frameTime)*toMs
					, double(min)*toMs
					, double(max)*toMs
					, freq/frameTime
					);

				const uint32_t msaa = (m_resolution.m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT;
				tvm.printf(10, pos++, 0x8e, " Reset flags: [%c] vsync, [%c] MSAAx%d, [%c] MaxAnisotropy "
					, !!(m_resolution.m_flags&BGFX_RESET_VSYNC) ? '\xfe' : ' '
					, 0 != msaa ? '\xfe' : ' '
					, 1<<msaa
					, !!(m_resolution.m_flags&BGFX_RESET_MAXANISOTROPY) ? '\xfe' : ' '
					);

				double elapsedCpuMs = double(elapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "   Submitted: %5d (draw %5d, compute %4d) / CPU %7.4f [ms] %c GPU %7.4f [ms] (latency %d)"
					, _render->m_num
					, statsKeyType[0]
					, statsKeyType[1]
					, elapsedCpuMs
					, elapsedCpuMs > maxGpuElapsed ? '>' : '<'
					, maxGpuElapsed
					, maxGpuLatency
					);
				maxGpuLatency = 0;
				maxGpuElapsed = 0.0;

				for (uint32_t ii = 0; ii < BX_COUNTOF(s_primName); ++ii)
				{
					tvm.printf(10, pos++, 0x8e, "   %9s: %7d (#inst: %5d), submitted: %7d"
						, s_primName[ii]
						, statsNumPrimsRendered[ii]
						, statsNumInstances[ii]
						, statsNumPrimsSubmitted[ii]
						);
				}

				tvm.printf(10, pos++, 0x8e, "      Indices: %7d ", statsNumIndices);
				tvm.printf(10, pos++, 0x8e, " Uniform size: %7d ", _render->m_constEnd);
				tvm.printf(10, pos++, 0x8e, "     DVB size: %7d ", _render->m_vboffset);
				tvm.printf(10, pos++, 0x8e, "     DIB size: %7d ", _render->m_iboffset);

				double captureMs = double(captureElapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "     Capture: %7.4f [ms]", captureMs);

				uint8_t attr[2] = { 0x89, 0x8a };
				uint8_t attrIndex = _render->m_waitSubmit < _render->m_waitRender;

				tvm.printf(10, pos++, attr[attrIndex&1], " Submit wait: %7.4f [ms]", _render->m_waitSubmit*toMs);
				tvm.printf(10, pos++, attr[(attrIndex+1)&1], " Render wait: %7.4f [ms]", _render->m_waitRender*toMs);

				min = frameTime;
				max = frameTime;
			}

			blit(this, _textVideoMemBlitter, tvm);

			PIX_ENDEVENT();
		}
		else if (_render->m_debug & BGFX_DEBUG_TEXT)
		{
			PIX_BEGINEVENT(D3DCOLOR_RGBA(0x40, 0x40, 0x40, 0xff), L"debugtext");

			blit(this, _textVideoMemBlitter, _render->m_textVideoMem);

			PIX_ENDEVENT();
		}

		device->EndScene();
	}
} /* namespace d3d9 */ } // namespace bgfx

#else

namespace bgfx { namespace d3d9
{
	RendererContextI* rendererCreate()
	{
		return NULL;
	}

	void rendererDestroy()
	{
	}
} /* namespace d3d9 */ } // namespace bgfx

#endif // BGFX_CONFIG_RENDERER_DIRECT3D9
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)
#	include "renderer_gl.h"
#	include <bx/timer.h>
#	include <bx/uint32_t.h>

namespace bgfx { namespace gl
{
	static char s_viewName[BGFX_CONFIG_MAX_VIEWS][BGFX_CONFIG_MAX_VIEW_NAME];

	struct PrimInfo
	{
		GLenum m_type;
		uint32_t m_min;
		uint32_t m_div;
		uint32_t m_sub;
	};

	static const PrimInfo s_primInfo[] =
	{
		{ GL_TRIANGLES,      3, 3, 0 },
		{ GL_TRIANGLE_STRIP, 3, 1, 2 },
		{ GL_LINES,          2, 2, 0 },
		{ GL_LINE_STRIP,     2, 1, 1 },
		{ GL_POINTS,         1, 1, 0 },
	};

	static const char* s_primName[] =
	{
		"TriList",
		"TriStrip",
		"Line",
		"LineStrip",
		"Point",
	};

	static const char* s_attribName[] =
	{
		"a_position",
		"a_normal",
		"a_tangent",
		"a_bitangent",
		"a_color0",
		"a_color1",
		"a_indices",
		"a_weight",
		"a_texcoord0",
		"a_texcoord1",
		"a_texcoord2",
		"a_texcoord3",
		"a_texcoord4",
		"a_texcoord5",
		"a_texcoord6",
		"a_texcoord7",
	};
	BX_STATIC_ASSERT(Attrib::Count == BX_COUNTOF(s_attribName) );

	static const char* s_instanceDataName[] =
	{
		"i_data0",
		"i_data1",
		"i_data2",
		"i_data3",
		"i_data4",
	};
	BX_STATIC_ASSERT(BGFX_CONFIG_MAX_INSTANCE_DATA_COUNT == BX_COUNTOF(s_instanceDataName) );

	static const GLenum s_access[] =
	{
		GL_READ_ONLY,
		GL_WRITE_ONLY,
		GL_READ_WRITE,
	};
	BX_STATIC_ASSERT(Access::Count == BX_COUNTOF(s_access) );

	static const GLenum s_attribType[] =
	{
		GL_UNSIGNED_BYTE,            // Uint8
		GL_UNSIGNED_INT_10_10_10_2,  // Uint10
		GL_SHORT,                    // Int16
		GL_HALF_FLOAT,               // Half
		GL_FLOAT,                    // Float
	};
	BX_STATIC_ASSERT(AttribType::Count == BX_COUNTOF(s_attribType) );

	struct Blend
	{
		GLenum m_src;
		GLenum m_dst;
		bool m_factor;
	};

	static const Blend s_blendFactor[] =
	{
		{ 0,                           0,                           false }, // ignored
		{ GL_ZERO,                     GL_ZERO,                     false }, // ZERO
		{ GL_ONE,                      GL_ONE,                      false }, // ONE
		{ GL_SRC_COLOR,                GL_SRC_COLOR,                false }, // SRC_COLOR
		{ GL_ONE_MINUS_SRC_COLOR,      GL_ONE_MINUS_SRC_COLOR,      false }, // INV_SRC_COLOR
		{ GL_SRC_ALPHA,                GL_SRC_ALPHA,                false }, // SRC_ALPHA
		{ GL_ONE_MINUS_SRC_ALPHA,      GL_ONE_MINUS_SRC_ALPHA,      false }, // INV_SRC_ALPHA
		{ GL_DST_ALPHA,                GL_DST_ALPHA,                false }, // DST_ALPHA
		{ GL_ONE_MINUS_DST_ALPHA,      GL_ONE_MINUS_DST_ALPHA,      false }, // INV_DST_ALPHA
		{ GL_DST_COLOR,                GL_DST_COLOR,                false }, // DST_COLOR
		{ GL_ONE_MINUS_DST_COLOR,      GL_ONE_MINUS_DST_COLOR,      false }, // INV_DST_COLOR
		{ GL_SRC_ALPHA_SATURATE,       GL_ONE,                      false }, // SRC_ALPHA_SAT
		{ GL_CONSTANT_COLOR,           GL_CONSTANT_COLOR,           true  }, // FACTOR
		{ GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, true  }, // INV_FACTOR
	};

	static const GLenum s_blendEquation[] =
	{
		GL_FUNC_ADD,
		GL_FUNC_SUBTRACT,
		GL_FUNC_REVERSE_SUBTRACT,
		GL_MIN,
		GL_MAX,
	};

	static const GLenum s_cmpFunc[] =
	{
		0, // ignored
		GL_LESS,
		GL_LEQUAL,
		GL_EQUAL,
		GL_GEQUAL,
		GL_GREATER,
		GL_NOTEQUAL,
		GL_NEVER,
		GL_ALWAYS,
	};

	static const GLenum s_stencilOp[] =
	{
		GL_ZERO,
		GL_KEEP,
		GL_REPLACE,
		GL_INCR_WRAP,
		GL_INCR,
		GL_DECR_WRAP,
		GL_DECR,
		GL_INVERT,
	};

	static const GLenum s_stencilFace[] =
	{
		GL_FRONT_AND_BACK,
		GL_FRONT,
		GL_BACK,
	};

	static const GLenum s_textureAddress[] =
	{
		GL_REPEAT,
		GL_MIRRORED_REPEAT,
		GL_CLAMP_TO_EDGE,
	};

	static const GLenum s_textureFilterMag[] =
	{
		GL_LINEAR,
		GL_NEAREST,
		GL_LINEAR,
	};

	static const GLenum s_textureFilterMin[][3] =
	{
		{ GL_LINEAR,  GL_LINEAR_MIPMAP_LINEAR,  GL_NEAREST_MIPMAP_LINEAR  },
		{ GL_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_NEAREST },
		{ GL_LINEAR,  GL_LINEAR_MIPMAP_LINEAR,  GL_NEAREST_MIPMAP_LINEAR  },
	};

	struct TextureFormatInfo
	{
		GLenum m_internalFmt;
		GLenum m_internalFmtSrgb;
		GLenum m_fmt;
		GLenum m_type;
		bool m_supported;
	};

	static TextureFormatInfo s_textureFormat[] =
	{
		{ GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT,        GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,            GL_ZERO,                         false }, // BC1
		{ GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT,        GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,            GL_ZERO,                         false }, // BC2
		{ GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,            GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT,        GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,            GL_ZERO,                         false }, // BC3
		{ GL_COMPRESSED_LUMINANCE_LATC1_EXT,           GL_ZERO,                                       GL_COMPRESSED_LUMINANCE_LATC1_EXT,           GL_ZERO,                         false }, // BC4
		{ GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT,     GL_ZERO,                                       GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT,     GL_ZERO,                         false }, // BC5
		{ GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB,     GL_ZERO,                                       GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB,     GL_ZERO,                         false }, // BC6H
		{ GL_COMPRESSED_RGBA_BPTC_UNORM_ARB,           GL_ZERO,                                       GL_COMPRESSED_RGBA_BPTC_UNORM_ARB,           GL_ZERO,                         false }, // BC7
		{ GL_ETC1_RGB8_OES,                            GL_ZERO,                                       GL_ETC1_RGB8_OES,                            GL_ZERO,                         false }, // ETC1
		{ GL_COMPRESSED_RGB8_ETC2,                     GL_ZERO,                                       GL_COMPRESSED_RGB8_ETC2,                     GL_ZERO,                         false }, // ETC2
		{ GL_COMPRESSED_RGBA8_ETC2_EAC,                GL_COMPRESSED_SRGB8_ETC2,                      GL_COMPRESSED_RGBA8_ETC2_EAC,                GL_ZERO,                         false }, // ETC2A
		{ GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2,  GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_ZERO,                         false }, // ETC2A1
		{ GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG,          GL_COMPRESSED_SRGB_PVRTC_2BPPV1_EXT,           GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG,          GL_ZERO,                         false }, // PTC12
		{ GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG,          GL_COMPRESSED_SRGB_PVRTC_4BPPV1_EXT,           GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG,          GL_ZERO,                         false }, // PTC14
		{ GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG,         GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV1_EXT,     GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG,         GL_ZERO,                         false }, // PTC12A
		{ GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,         GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV1_EXT,     GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG,         GL_ZERO,                         false }, // PTC14A
		{ GL_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG,         GL_ZERO,                                       GL_COMPRESSED_RGBA_PVRTC_2BPPV2_IMG,         GL_ZERO,                         false }, // PTC22
		{ GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG,         GL_ZERO,                                       GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG,         GL_ZERO,                         false }, // PTC24
		{ GL_ZERO,                                     GL_ZERO,                                       GL_ZERO,                                     GL_ZERO,                         false }, // Unknown
		{ GL_ZERO,                                     GL_ZERO,                                       GL_ZERO,                                     GL_ZERO,                         false }, // R1
		{ GL_R8,                                       GL_ZERO,                                       GL_RED,                                      GL_UNSIGNED_BYTE,                false }, // R8
		{ GL_R8I,                                      GL_ZERO,                                       GL_RED,                                      GL_BYTE,                         false }, // R8S
		{ GL_R8UI,                                     GL_ZERO,                                       GL_RED,                                      GL_UNSIGNED_BYTE,                false }, // R8S
		{ GL_R8_SNORM,                                 GL_ZERO,                                       GL_RED,                                      GL_BYTE,                         false }, // R8S
		{ GL_R16,                                      GL_ZERO,                                       GL_RED,                                      GL_UNSIGNED_SHORT,               false }, // R16
		{ GL_R16I,                                     GL_ZERO,                                       GL_RED,                                      GL_SHORT,                        false }, // R16I
		{ GL_R16UI,                                    GL_ZERO,                                       GL_RED,                                      GL_UNSIGNED_SHORT,               false }, // R16U
		{ GL_R16F,                                     GL_ZERO,                                       GL_RED,                                      GL_HALF_FLOAT,                   false }, // R16F
		{ GL_R16_SNORM,                                GL_ZERO,                                       GL_RED,                                      GL_SHORT,                        false }, // R16S
		{ GL_R32UI,                                    GL_ZERO,                                       GL_RED,                                      GL_UNSIGNED_INT,                 false }, // R32U
		{ GL_R32F,                                     GL_ZERO,                                       GL_RED,                                      GL_FLOAT,                        false }, // R32F
		{ GL_RG8,                                      GL_ZERO,                                       GL_RG,                                       GL_UNSIGNED_BYTE,                false }, // RG8
		{ GL_RG8I,                                     GL_ZERO,                                       GL_RG,                                       GL_BYTE,                         false }, // RG8I
		{ GL_RG8UI,                                    GL_ZERO,                                       GL_RG,                                       GL_UNSIGNED_BYTE,                false }, // RG8U
		{ GL_RG8_SNORM,                                GL_ZERO,                                       GL_RG,                                       GL_BYTE,                         false }, // RG8S
		{ GL_RG16,                                     GL_ZERO,                                       GL_RG,                                       GL_UNSIGNED_SHORT,               false }, // RG16
		{ GL_RG16I,                                    GL_ZERO,                                       GL_RG,                                       GL_SHORT,                        false }, // RG16
		{ GL_RG16UI,                                   GL_ZERO,                                       GL_RG,                                       GL_UNSIGNED_SHORT,               false }, // RG16
		{ GL_RG16F,                                    GL_ZERO,                                       GL_RG,                                       GL_FLOAT,                        false }, // RG16F
		{ GL_RG16_SNORM,                               GL_ZERO,                                       GL_RG,                                       GL_SHORT,                        false }, // RG16S
		{ GL_RG32UI,                                   GL_ZERO,                                       GL_RG,                                       GL_UNSIGNED_INT,                 false }, // RG32U
		{ GL_RG32F,                                    GL_ZERO,                                       GL_RG,                                       GL_FLOAT,                        false }, // RG32F
		{ GL_RGBA8,                                    GL_SRGB8_ALPHA8,                               GL_BGRA,                                     GL_UNSIGNED_BYTE,                false }, // BGRA8
		{ GL_RGBA8,                                    GL_SRGB8_ALPHA8,                               GL_RGBA,                                     GL_UNSIGNED_BYTE,                false }, // RGBA8
		{ GL_RGBA8I,                                   GL_ZERO,                                       GL_RGBA,                                     GL_BYTE,                         false }, // RGBA8I
		{ GL_RGBA8UI,                                  GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_BYTE,                false }, // RGBA8U
		{ GL_RGBA8_SNORM,                              GL_ZERO,                                       GL_RGBA,                                     GL_BYTE,                         false }, // RGBA8S
		{ GL_RGBA16,                                   GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_SHORT,               false }, // RGBA16
		{ GL_RGBA16I,                                  GL_ZERO,                                       GL_RGBA,                                     GL_SHORT,                        false }, // RGBA16I
		{ GL_RGBA16UI,                                 GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_SHORT,               false }, // RGBA16U
		{ GL_RGBA16F,                                  GL_ZERO,                                       GL_RGBA,                                     GL_HALF_FLOAT,                   false }, // RGBA16F
		{ GL_RGBA16_SNORM,                             GL_ZERO,                                       GL_RGBA,                                     GL_SHORT,                        false }, // RGBA16S
		{ GL_RGBA32UI,                                 GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_INT,                 false }, // RGBA32U
		{ GL_RGBA32F,                                  GL_ZERO,                                       GL_RGBA,                                     GL_FLOAT,                        false }, // RGBA32F
		{ GL_RGB565,                                   GL_ZERO,                                       GL_RGB,                                      GL_UNSIGNED_SHORT_5_6_5,         false }, // R5G6B5
		{ GL_RGBA4,                                    GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_SHORT_4_4_4_4,       false }, // RGBA4
		{ GL_RGB5_A1,                                  GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_SHORT_5_5_5_1,       false }, // RGB5A1
		{ GL_RGB10_A2,                                 GL_ZERO,                                       GL_RGBA,                                     GL_UNSIGNED_INT_2_10_10_10_REV,  false }, // RGB10A2
		{ GL_R11F_G11F_B10F,                           GL_ZERO,                                       GL_RGB,                                      GL_UNSIGNED_INT_10F_11F_11F_REV, false }, // R11G11B10F
		{ GL_ZERO,                                     GL_ZERO,                                       GL_ZERO,                                     GL_ZERO,                         false }, // UnknownDepth
		{ GL_DEPTH_COMPONENT16,                        GL_ZERO,                                       GL_DEPTH_COMPONENT,                          GL_UNSIGNED_SHORT,               false }, // D16
		{ GL_DEPTH_COMPONENT24,                        GL_ZERO,                                       GL_DEPTH_COMPONENT,                          GL_UNSIGNED_INT,                 false }, // D24
		{ GL_DEPTH24_STENCIL8,                         GL_ZERO,                                       GL_DEPTH_STENCIL,                            GL_UNSIGNED_INT_24_8,            false }, // D24S8
		{ GL_DEPTH_COMPONENT32,                        GL_ZERO,                                       GL_DEPTH_COMPONENT,                          GL_UNSIGNED_INT,                 false }, // D32
		{ GL_DEPTH_COMPONENT32F,                       GL_ZERO,                                       GL_DEPTH_COMPONENT,                          GL_FLOAT,                        false }, // D16F
		{ GL_DEPTH_COMPONENT32F,                       GL_ZERO,                                       GL_DEPTH_COMPONENT,                          GL_FLOAT,                        false }, // D24F
		{ GL_DEPTH_COMPONENT32F,                       GL_ZERO,                                       GL_DEPTH_COMPONENT,                          GL_FLOAT,                        false }, // D32F
		{ GL_STENCIL_INDEX8,                           GL_ZERO,                                       GL_STENCIL_INDEX,                            GL_UNSIGNED_BYTE,                false }, // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_textureFormat) );

	static bool s_textureFilter[TextureFormat::Count+1];

	static GLenum s_rboFormat[] =
	{
		GL_ZERO,               // BC1
		GL_ZERO,               // BC2
		GL_ZERO,               // BC3
		GL_ZERO,               // BC4
		GL_ZERO,               // BC5
		GL_ZERO,               // BC6H
		GL_ZERO,               // BC7
		GL_ZERO,               // ETC1
		GL_ZERO,               // ETC2
		GL_ZERO,               // ETC2A
		GL_ZERO,               // ETC2A1
		GL_ZERO,               // PTC12
		GL_ZERO,               // PTC14
		GL_ZERO,               // PTC12A
		GL_ZERO,               // PTC14A
		GL_ZERO,               // PTC22
		GL_ZERO,               // PTC24
		GL_ZERO,               // Unknown
		GL_ZERO,               // R1
		GL_R8,                 // R8
		GL_R8I,                // R8I
		GL_R8UI,               // R8U
		GL_R8_SNORM,           // R8S
		GL_R16,                // R16
		GL_R16I,               // R16I
		GL_R16UI,              // R16U
		GL_R16F,               // R16F
		GL_R16_SNORM,          // R16S
		GL_R32UI,              // R32U
		GL_R32F,               // R32F
		GL_RG8,                // RG8
		GL_RG8I,               // RG8I
		GL_RG8UI,              // RG8U
		GL_RG8_SNORM,          // RG8S
		GL_RG16,               // RG16
		GL_RG16I,              // RG16I
		GL_RG16UI,             // RG16U
		GL_RG16F,              // RG16F
		GL_RG16_SNORM,         // RG16S
		GL_RG32UI,             // RG32U
		GL_RG32F,              // RG32F
		GL_RGBA8,              // BGRA8
		GL_RGBA8,              // RGBA8
		GL_RGBA8I,             // RGBA8I
		GL_RGBA8UI,            // RGBA8UI
		GL_RGBA8_SNORM,        // RGBA8S
		GL_RGBA16,             // RGBA16
		GL_RGBA16I,            // RGBA16I
		GL_RGBA16UI,           // RGBA16U
		GL_RGBA16F,            // RGBA16F
		GL_RGBA16_SNORM,       // RGBA16S
		GL_RGBA32UI,           // RGBA32U
		GL_RGBA32F,            // RGBA32F
		GL_RGB565,             // R5G6B5
		GL_RGBA4,              // RGBA4
		GL_RGB5_A1,            // RGB5A1
		GL_RGB10_A2,           // RGB10A2
		GL_R11F_G11F_B10F,     // R11G11B10F
		GL_ZERO,               // UnknownDepth
		GL_DEPTH_COMPONENT16,  // D16
		GL_DEPTH_COMPONENT24,  // D24
		GL_DEPTH24_STENCIL8,   // D24S8
		GL_DEPTH_COMPONENT32,  // D32
		GL_DEPTH_COMPONENT32F, // D16F
		GL_DEPTH_COMPONENT32F, // D24F
		GL_DEPTH_COMPONENT32F, // D32F
		GL_STENCIL_INDEX8,     // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_rboFormat) );

	static GLenum s_imageFormat[] =
	{
		GL_ZERO,           // BC1
		GL_ZERO,           // BC2
		GL_ZERO,           // BC3
		GL_ZERO,           // BC4
		GL_ZERO,           // BC5
		GL_ZERO,           // BC6H
		GL_ZERO,           // BC7
		GL_ZERO,           // ETC1
		GL_ZERO,           // ETC2
		GL_ZERO,           // ETC2A
		GL_ZERO,           // ETC2A1
		GL_ZERO,           // PTC12
		GL_ZERO,           // PTC14
		GL_ZERO,           // PTC12A
		GL_ZERO,           // PTC14A
		GL_ZERO,           // PTC22
		GL_ZERO,           // PTC24
		GL_ZERO,           // Unknown
		GL_ZERO,           // R1
		GL_R8,             // R8
		GL_R8I,            // R8I
		GL_R8UI,           // R8UI
		GL_R8_SNORM,       // R8S
		GL_R16,            // R16
		GL_R16I,           // R16I
		GL_R16UI,          // R16U
		GL_R16F,           // R16F
		GL_R16_SNORM,      // R16S
		GL_R32UI,          // R32U
		GL_R32F,           // R32F
		GL_RG8,            // RG8
		GL_RG8I,           // RG8I
		GL_RG8UI,          // RG8U
		GL_RG8_SNORM,      // RG8S
		GL_RG16,           // RG16
		GL_RG16I,          // RG16I
		GL_RG16UI,         // RG16U
		GL_RG16F,          // RG16F
		GL_RG16_SNORM,     // RG16S
		GL_RG32UI,         // RG32U
		GL_RG32F,          // RG32F
		GL_RGBA8,          // BGRA8
		GL_RGBA8,          // RGBA8
		GL_RGBA8I,         // RGBA8I
		GL_RGBA8UI,        // RGBA8UI
		GL_RGBA8_SNORM,    // RGBA8S
		GL_RGBA16,         // RGBA16
		GL_RGBA16I,        // RGBA16I
		GL_RGBA16UI,       // RGBA16U
		GL_RGBA16F,        // RGBA16F
		GL_RGBA16_SNORM,   // RGBA16S
		GL_RGBA32UI,       // RGBA32U
		GL_RGBA32F,        // RGBA32F
		GL_RGB565,         // R5G6B5
		GL_RGBA4,          // RGBA4
		GL_RGB5_A1,        // RGB5A1
		GL_RGB10_A2,       // RGB10A2
		GL_R11F_G11F_B10F, // R11G11B10F
		GL_ZERO,           // UnknownDepth
		GL_ZERO,           // D16
		GL_ZERO,           // D24
		GL_ZERO,           // D24S8
		GL_ZERO,           // D32
		GL_ZERO,           // D16F
		GL_ZERO,           // D24F
		GL_ZERO,           // D32F
		GL_ZERO,           // D0S8
	};
	BX_STATIC_ASSERT(TextureFormat::Count == BX_COUNTOF(s_imageFormat) );

	struct Extension
	{
		enum Enum
		{
			AMD_conservative_depth,
			AMD_multi_draw_indirect,

			ANGLE_depth_texture,
			ANGLE_framebuffer_blit,
			ANGLE_framebuffer_multisample,
			ANGLE_instanced_arrays,
			ANGLE_texture_compression_dxt1,
			ANGLE_texture_compression_dxt3,
			ANGLE_texture_compression_dxt5,
			ANGLE_timer_query,
			ANGLE_translated_shader_source,

			APPLE_texture_format_BGRA8888,
			APPLE_texture_max_level,

			ARB_compute_shader,
			ARB_conservative_depth,
			ARB_debug_label,
			ARB_debug_output,
			ARB_depth_buffer_float,
			ARB_depth_clamp,
			ARB_draw_buffers_blend,
			ARB_draw_indirect,
			ARB_draw_instanced,
			ARB_ES3_compatibility,
			ARB_framebuffer_object,
			ARB_framebuffer_sRGB,
			ARB_get_program_binary,
			ARB_half_float_pixel,
			ARB_half_float_vertex,
			ARB_instanced_arrays,
			ARB_invalidate_subdata,
			ARB_map_buffer_range,
			ARB_multi_draw_indirect,
			ARB_multisample,
			ARB_occlusion_query,
			ARB_occlusion_query2,
			ARB_program_interface_query,
			ARB_sampler_objects,
			ARB_seamless_cube_map,
			ARB_shader_bit_encoding,
			ARB_shader_image_load_store,
			ARB_shader_storage_buffer_object,
			ARB_shader_texture_lod,
			ARB_texture_compression_bptc,
			ARB_texture_compression_rgtc,
			ARB_texture_float,
			ARB_texture_multisample,
			ARB_texture_rg,
			ARB_texture_rgb10_a2ui,
			ARB_texture_stencil8,
			ARB_texture_storage,
			ARB_texture_swizzle,
			ARB_timer_query,
			ARB_uniform_buffer_object,
			ARB_vertex_array_object,
			ARB_vertex_type_2_10_10_10_rev,

			ATI_meminfo,

			CHROMIUM_color_buffer_float_rgb,
			CHROMIUM_color_buffer_float_rgba,
			CHROMIUM_depth_texture,
			CHROMIUM_framebuffer_multisample,
			CHROMIUM_texture_compression_dxt3,
			CHROMIUM_texture_compression_dxt5,

			EXT_bgra,
			EXT_blend_color,
			EXT_blend_minmax,
			EXT_blend_subtract,
			EXT_color_buffer_half_float,
			EXT_color_buffer_float,
			EXT_compressed_ETC1_RGB8_sub_texture,
			EXT_debug_label,
			EXT_debug_marker,
			EXT_debug_tool,
			EXT_discard_framebuffer,
			EXT_disjoint_timer_query,
			EXT_draw_buffers,
			EXT_frag_depth,
			EXT_framebuffer_blit,
			EXT_framebuffer_object,
			EXT_framebuffer_sRGB,
			EXT_multi_draw_indirect,
			EXT_occlusion_query_boolean,
			EXT_packed_float,
			EXT_read_format_bgra,
			EXT_shader_image_load_store,
			EXT_shader_texture_lod,
			EXT_shadow_samplers,
			EXT_texture_array,
			EXT_texture_compression_dxt1,
			EXT_texture_compression_latc,
			EXT_texture_compression_rgtc,
			EXT_texture_compression_s3tc,
			EXT_texture_filter_anisotropic,
			EXT_texture_format_BGRA8888,
			EXT_texture_rg,
			EXT_texture_snorm,
			EXT_texture_sRGB,
			EXT_texture_storage,
			EXT_texture_swizzle,
			EXT_texture_type_2_10_10_10_REV,
			EXT_timer_query,
			EXT_unpack_subimage,

			GOOGLE_depth_texture,

			GREMEDY_string_marker,
			GREMEDY_frame_terminator,

			IMG_multisampled_render_to_texture,
			IMG_read_format,
			IMG_shader_binary,
			IMG_texture_compression_pvrtc,
			IMG_texture_compression_pvrtc2,
			IMG_texture_format_BGRA8888,

			INTEL_fragment_shader_ordering,

			KHR_debug,
			KHR_no_error,

			MOZ_WEBGL_compressed_texture_s3tc,
			MOZ_WEBGL_depth_texture,

			NV_draw_buffers,
			NVX_gpu_memory_info,

			OES_compressed_ETC1_RGB8_texture,
			OES_depth24,
			OES_depth32,
			OES_depth_texture,
			OES_element_index_uint,
			OES_fragment_precision_high,
			OES_get_program_binary,
			OES_required_internalformat,
			OES_packed_depth_stencil,
			OES_read_format,
			OES_rgb8_rgba8,
			OES_standard_derivatives,
			OES_texture_3D,
			OES_texture_float,
			OES_texture_float_linear,
			OES_texture_npot,
			OES_texture_half_float,
			OES_texture_half_float_linear,
			OES_texture_stencil8,
			OES_vertex_array_object,
			OES_vertex_half_float,
			OES_vertex_type_10_10_10_2,

			WEBGL_color_buffer_float,
			WEBGL_compressed_texture_etc1,
			WEBGL_compressed_texture_s3tc,
			WEBGL_compressed_texture_pvrtc,
			WEBGL_depth_texture,
			WEBGL_draw_buffers,

			WEBKIT_EXT_texture_filter_anisotropic,
			WEBKIT_WEBGL_compressed_texture_s3tc,
			WEBKIT_WEBGL_depth_texture,

			Count
		};

		const char* m_name;
		bool m_supported;
		bool m_initialize;
	};

	// Extension registry
	//
	// ANGLE:
	// https://github.com/google/angle/tree/master/extensions
	//
	// CHROMIUM:
	// https://chromium.googlesource.com/chromium/src.git/+/refs/heads/git-svn/gpu/GLES2/extensions/CHROMIUM
	//
	// EGL:
	// https://www.khronos.org/registry/egl/extensions/
	//
	// GL:
	// https://www.opengl.org/registry/
	//
	// GLES:
	// https://www.khronos.org/registry/gles/extensions/
	//
	// WEBGL:
	// https://www.khronos.org/registry/webgl/extensions/
	//
	static Extension s_extension[] =
	{
		{ "AMD_conservative_depth",                false,                             true  },
		{ "AMD_multi_draw_indirect",               false,                             true  },

		{ "ANGLE_depth_texture",                   false,                             true  },
		{ "ANGLE_framebuffer_blit",                false,                             true  },
		{ "ANGLE_framebuffer_multisample",         false,                             false },
		{ "ANGLE_instanced_arrays",                false,                             true  },
		{ "ANGLE_texture_compression_dxt1",        false,                             true  },
		{ "ANGLE_texture_compression_dxt3",        false,                             true  },
		{ "ANGLE_texture_compression_dxt5",        false,                             true  },
		{ "ANGLE_timer_query",                     false,                             true  },
		{ "ANGLE_translated_shader_source",        false,                             true  },

		{ "APPLE_texture_format_BGRA8888",         false,                             true  },
		{ "APPLE_texture_max_level",               false,                             true  },

		{ "ARB_compute_shader",                    BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_conservative_depth",                BGFX_CONFIG_RENDERER_OPENGL >= 42, true  },
		{ "ARB_debug_label",                       false,                             true  },
		{ "ARB_debug_output",                      BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_depth_buffer_float",                BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_depth_clamp",                       BGFX_CONFIG_RENDERER_OPENGL >= 32, true  },
		{ "ARB_draw_buffers_blend",                BGFX_CONFIG_RENDERER_OPENGL >= 40, true  },
		{ "ARB_draw_indirect",                     BGFX_CONFIG_RENDERER_OPENGL >= 40, true  },
		{ "ARB_draw_instanced",                    BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_ES3_compatibility",                 BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_framebuffer_object",                BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_framebuffer_sRGB",                  BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_get_program_binary",                BGFX_CONFIG_RENDERER_OPENGL >= 41, true  },
		{ "ARB_half_float_pixel",                  BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_half_float_vertex",                 BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_instanced_arrays",                  BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_invalidate_subdata",                BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_map_buffer_range",                  BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_multi_draw_indirect",               BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_multisample",                       false,                             true  },
		{ "ARB_occlusion_query",                   BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_occlusion_query2",                  BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_program_interface_query",           BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_sampler_objects",                   BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_seamless_cube_map",                 BGFX_CONFIG_RENDERER_OPENGL >= 32, true  },
		{ "ARB_shader_bit_encoding",               BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_shader_image_load_store",           BGFX_CONFIG_RENDERER_OPENGL >= 42, true  },
		{ "ARB_shader_storage_buffer_object",      BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "ARB_shader_texture_lod",                BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_texture_compression_bptc",          BGFX_CONFIG_RENDERER_OPENGL >= 44, true  },
		{ "ARB_texture_compression_rgtc",          BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_texture_float",                     BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_texture_multisample",               BGFX_CONFIG_RENDERER_OPENGL >= 32, true  },
		{ "ARB_texture_rg",                        BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_texture_rgb10_a2ui",                BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_texture_stencil8",                  false,                             true  },
		{ "ARB_texture_storage",                   BGFX_CONFIG_RENDERER_OPENGL >= 42, true  },
		{ "ARB_texture_swizzle",                   BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_timer_query",                       BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "ARB_uniform_buffer_object",             BGFX_CONFIG_RENDERER_OPENGL >= 31, true  },
		{ "ARB_vertex_array_object",               BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "ARB_vertex_type_2_10_10_10_rev",        false,                             true  },

		{ "ATI_meminfo",                           false,                             true  },

		{ "CHROMIUM_color_buffer_float_rgb",       false,                             true  },
		{ "CHROMIUM_color_buffer_float_rgba",      false,                             true  },
		{ "CHROMIUM_depth_texture",                false,                             true  },
		{ "CHROMIUM_framebuffer_multisample",      false,                             true  },
		{ "CHROMIUM_texture_compression_dxt3",     false,                             true  },
		{ "CHROMIUM_texture_compression_dxt5",     false,                             true  },

		{ "EXT_bgra",                              false,                             true  },
		{ "EXT_blend_color",                       BGFX_CONFIG_RENDERER_OPENGL >= 31, true  },
		{ "EXT_blend_minmax",                      BGFX_CONFIG_RENDERER_OPENGL >= 14, true  },
		{ "EXT_blend_subtract",                    BGFX_CONFIG_RENDERER_OPENGL >= 14, true  },
		{ "EXT_color_buffer_half_float",           false,                             true  }, // GLES2 extension.
		{ "EXT_color_buffer_float",                false,                             true  }, // GLES2 extension.
		{ "EXT_compressed_ETC1_RGB8_sub_texture",  false,                             true  }, // GLES2 extension.
		{ "EXT_debug_label",                       false,                             true  },
		{ "EXT_debug_marker",                      false,                             true  },
		{ "EXT_debug_tool",                        false,                             true  }, // RenderDoc extension.
		{ "EXT_discard_framebuffer",               false,                             true  }, // GLES2 extension.
		{ "EXT_disjoint_timer_query",              false,                             true  }, // GLES2 extension.
		{ "EXT_draw_buffers",                      false,                             true  }, // GLES2 extension.
		{ "EXT_frag_depth",                        false,                             true  }, // GLES2 extension.
		{ "EXT_framebuffer_blit",                  BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "EXT_framebuffer_object",                BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "EXT_framebuffer_sRGB",                  BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "EXT_multi_draw_indirect",               false,                             true  }, // GLES3.1 extension.
		{ "EXT_occlusion_query_boolean",           false,                             true  },
		{ "EXT_packed_float",                      BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "EXT_read_format_bgra",                  false,                             true  },
		{ "EXT_shader_image_load_store",           false,                             true  },
		{ "EXT_shader_texture_lod",                false,                             true  }, // GLES2 extension.
		{ "EXT_shadow_samplers",                   false,                             true  },
		{ "EXT_texture_array",                     BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "EXT_texture_compression_dxt1",          false,                             true  },
		{ "EXT_texture_compression_latc",          false,                             true  },
		{ "EXT_texture_compression_rgtc",          BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "EXT_texture_compression_s3tc",          false,                             true  },
		{ "EXT_texture_filter_anisotropic",        false,                             true  },
		{ "EXT_texture_format_BGRA8888",           false,                             true  },
		{ "EXT_texture_rg",                        false,                             true  }, // GLES2 extension.
		{ "EXT_texture_snorm",                     BGFX_CONFIG_RENDERER_OPENGL >= 30, true  },
		{ "EXT_texture_sRGB",                      false,                             true  },
		{ "EXT_texture_storage",                   false,                             true  },
		{ "EXT_texture_swizzle",                   false,                             true  },
		{ "EXT_texture_type_2_10_10_10_REV",       false,                             true  },
		{ "EXT_timer_query",                       BGFX_CONFIG_RENDERER_OPENGL >= 33, true  },
		{ "EXT_unpack_subimage",                   false,                             true  },

		{ "GOOGLE_depth_texture",                  false,                             true  },

		{ "GREMEDY_string_marker",                 false,                             true  },
		{ "GREMEDY_frame_terminator",              false,                             true  },

		{ "IMG_multisampled_render_to_texture",    false,                             true  },
		{ "IMG_read_format",                       false,                             true  },
		{ "IMG_shader_binary",                     false,                             true  },
		{ "IMG_texture_compression_pvrtc",         false,                             true  },
		{ "IMG_texture_compression_pvrtc2",        false,                             true  },
		{ "IMG_texture_format_BGRA8888",           false,                             true  },

		{ "INTEL_fragment_shader_ordering",        false,                             true  },

		{ "KHR_debug",                             BGFX_CONFIG_RENDERER_OPENGL >= 43, true  },
		{ "KHR_no_error",                          false,                             true  },

		{ "MOZ_WEBGL_compressed_texture_s3tc",     false,                             true  },
		{ "MOZ_WEBGL_depth_texture",               false,                             true  },

		{ "NV_draw_buffers",                       false,                             true  }, // GLES2 extension.
		{ "NVX_gpu_memory_info",                   false,                             true  },

		{ "OES_compressed_ETC1_RGB8_texture",      false,                             true  },
		{ "OES_depth24",                           false,                             true  },
		{ "OES_depth32",                           false,                             true  },
		{ "OES_depth_texture",                     false,                             true  },
		{ "OES_element_index_uint",                false,                             true  },
		{ "OES_fragment_precision_high",           false,                             true  },
		{ "OES_get_program_binary",                false,                             true  },
		{ "OES_required_internalformat",           false,                             true  },
		{ "OES_packed_depth_stencil",              false,                             true  },
		{ "OES_read_format",                       false,                             true  },
		{ "OES_rgb8_rgba8",                        false,                             true  },
		{ "OES_standard_derivatives",              false,                             true  },
		{ "OES_texture_3D",                        false,                             true  },
		{ "OES_texture_float",                     false,                             true  },
		{ "OES_texture_float_linear",              false,                             true  },
		{ "OES_texture_npot",                      false,                             true  },
		{ "OES_texture_half_float",                false,                             true  },
		{ "OES_texture_half_float_linear",         false,                             true  },
		{ "OES_texture_stencil8",                  false,                             true  },
		{ "OES_vertex_array_object",               false,                             !BX_PLATFORM_IOS },
		{ "OES_vertex_half_float",                 false,                             true  },
		{ "OES_vertex_type_10_10_10_2",            false,                             true  },

		{ "WEBGL_color_buffer_float",              false,                             true  },
		{ "WEBGL_compressed_texture_etc1",         false,                             true  },
		{ "WEBGL_compressed_texture_s3tc",         false,                             true  },
		{ "WEBGL_compressed_texture_pvrtc",        false,                             true  },
		{ "WEBGL_depth_texture",                   false,                             true  },
		{ "WEBGL_draw_buffers",                    false,                             true  },

		{ "WEBKIT_EXT_texture_filter_anisotropic", false,                             true  },
		{ "WEBKIT_WEBGL_compressed_texture_s3tc",  false,                             true  },
		{ "WEBKIT_WEBGL_depth_texture",            false,                             true  },
	};
	BX_STATIC_ASSERT(Extension::Count == BX_COUNTOF(s_extension) );

	static const char* s_ARB_shader_texture_lod[] =
	{
		"texture2DLod",
		"texture2DProjLod",
		"texture3DLod",
		"texture3DProjLod",
		"textureCubeLod",
		"shadow2DLod",
		"shadow2DProjLod",
		NULL
		// "texture1DLod",
		// "texture1DProjLod",
		// "shadow1DLod",
		// "shadow1DProjLod",
	};

	static const char* s_EXT_shader_texture_lod[] =
	{
		"texture2DLod",
		"texture2DProjLod",
		"textureCubeLod",
		NULL
		// "texture2DGrad",
		// "texture2DProjGrad",
		// "textureCubeGrad",
	};

	static const char* s_EXT_shadow_samplers[] =
	{
		"shadow2D",
		"shadow2DProj",
		NULL
	};

	static const char* s_OES_standard_derivatives[] =
	{
		"dFdx",
		"dFdy",
		"fwidth",
		NULL
	};

	static const char* s_OES_texture_3D[] =
	{
		"texture3D",
		"texture3DProj",
		"texture3DLod",
		"texture3DProjLod",
		NULL
	};

	static const char* s_uisamplers[] =
	{
		"isampler2D",
		"usampler2D",
		"isampler3D",
		"usampler3D",
		"isamplerCube",
		"usamplerCube",
		NULL
	};

	static void GL_APIENTRY stubVertexAttribDivisor(GLuint /*_index*/, GLuint /*_divisor*/)
	{
	}

	static void GL_APIENTRY stubDrawArraysInstanced(GLenum _mode, GLint _first, GLsizei _count, GLsizei /*_primcount*/)
	{
		GL_CHECK(glDrawArrays(_mode, _first, _count) );
	}

	static void GL_APIENTRY stubDrawElementsInstanced(GLenum _mode, GLsizei _count, GLenum _type, const GLvoid* _indices, GLsizei /*_primcount*/)
	{
		GL_CHECK(glDrawElements(_mode, _count, _type, _indices) );
	}

	static void GL_APIENTRY stubFrameTerminatorGREMEDY()
	{
	}

	static void GL_APIENTRY stubInsertEventMarker(GLsizei /*_length*/, const char* /*_marker*/)
	{
	}

	static void GL_APIENTRY stubInsertEventMarkerGREMEDY(GLsizei _length, const char* _marker)
	{
		// If <marker> is a null-terminated string then <length> should not
		// include the terminator.
		//
		// If <length> is 0 then <marker> is assumed to be null-terminated.

		uint32_t size = (0 == _length ? (uint32_t)strlen(_marker) : _length) + 1;
		size *= sizeof(wchar_t);
		wchar_t* name = (wchar_t*)alloca(size);
		mbstowcs(name, _marker, size-2);
		GL_CHECK(glStringMarkerGREMEDY(_length, _marker) );
	}

	static void GL_APIENTRY stubObjectLabel(GLenum /*_identifier*/, GLuint /*_name*/, GLsizei /*_length*/, const char* /*_label*/)
	{
	}

	static void GL_APIENTRY stubInvalidateFramebuffer(GLenum /*_target*/, GLsizei /*_numAttachments*/, const GLenum* /*_attachments*/)
	{
	}

	static void GL_APIENTRY stubMultiDrawArraysIndirect(GLenum _mode, const void* _indirect, GLsizei _drawcount, GLsizei _stride)
	{
		const uint8_t* args = (const uint8_t*)_indirect;
		for (GLsizei ii = 0; ii < _drawcount; ++ii)
		{
			GL_CHECK(glDrawArraysIndirect(_mode, (void*)args) );
			args += _stride;
		}
	}

	static void GL_APIENTRY stubMultiDrawElementsIndirect(GLenum _mode, GLenum _type, const void* _indirect, GLsizei _drawcount, GLsizei _stride)
	{
		const uint8_t* args = (const uint8_t*)_indirect;
		for (GLsizei ii = 0; ii < _drawcount; ++ii)
		{
			GL_CHECK(glDrawElementsIndirect(_mode, _type, (void*)args) );
			args += _stride;
		}
	}

	typedef void (*PostSwapBuffersFn)(uint32_t _width, uint32_t _height);

	static const char* getGLString(GLenum _name)
	{
		const char* str = (const char*)glGetString(_name);
		glGetError(); // ignore error if glGetString returns NULL.
		if (NULL != str)
		{
			return str;
		}

		return "<unknown>";
	}

	static uint32_t getGLStringHash(GLenum _name)
	{
		const char* str = (const char*)glGetString(_name);
		glGetError(); // ignore error if glGetString returns NULL.
		if (NULL != str)
		{
			return bx::hashMurmur2A(str, (uint32_t)strlen(str) );
		}

		return 0;
	}

	void dumpExtensions(const char* _extensions)
	{
		if (NULL != _extensions)
		{
			char name[1024];
			const char* pos = _extensions;
			const char* end = _extensions + strlen(_extensions);
			while (pos < end)
			{
				uint32_t len;
				const char* space = strchr(pos, ' ');
				if (NULL != space)
				{
					len = bx::uint32_min(sizeof(name), (uint32_t)(space - pos) );
				}
				else
				{
					len = bx::uint32_min(sizeof(name), (uint32_t)strlen(pos) );
				}

				strncpy(name, pos, len);
				name[len] = '\0';

				BX_TRACE("\t%s", name);

				pos += len+1;
			}
		}
	}

	const char* toString(GLenum _enum)
	{
		switch (_enum)
		{
		case GL_DEBUG_SOURCE_API:               return "API";
		case GL_DEBUG_SOURCE_WINDOW_SYSTEM:     return "WinSys";
		case GL_DEBUG_SOURCE_SHADER_COMPILER:   return "Shader";
		case GL_DEBUG_SOURCE_THIRD_PARTY:       return "3rdparty";
		case GL_DEBUG_SOURCE_APPLICATION:       return "Application";
		case GL_DEBUG_SOURCE_OTHER:             return "Other";
		case GL_DEBUG_TYPE_ERROR:               return "Error";
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "Deprecated behavior";
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  return "Undefined behavior";
		case GL_DEBUG_TYPE_PORTABILITY:         return "Portability";
		case GL_DEBUG_TYPE_PERFORMANCE:         return "Performance";
		case GL_DEBUG_TYPE_OTHER:               return "Other";
		case GL_DEBUG_SEVERITY_HIGH:            return "High";
		case GL_DEBUG_SEVERITY_MEDIUM:          return "Medium";
		case GL_DEBUG_SEVERITY_LOW:             return "Low";
		default:
			break;
		}

		return "<unknown>";
	}

	void GL_APIENTRY debugProcCb(GLenum _source, GLenum _type, GLuint _id, GLenum _severity, GLsizei /*_length*/, const GLchar* _message, const void* /*_userParam*/)
	{
		BX_TRACE("src %s, type %s, id %d, severity %s, '%s'"
				, toString(_source)
				, toString(_type)
				, _id
				, toString(_severity)
				, _message
				);
		BX_UNUSED(_source, _type, _id, _severity, _message);
	}

	GLint glGet(GLenum _pname)
	{
		GLint result = 0;
		glGetIntegerv(_pname, &result);
		GLenum err = glGetError();
		BX_WARN(0 == err, "glGetIntegerv(0x%04x, ...) failed with GL error: 0x%04x.", _pname, err);
		return 0 == err ? result : 0;
	}

	void setTextureFormat(TextureFormat::Enum _format, GLenum _internalFmt, GLenum _fmt, GLenum _type = GL_ZERO)
	{
		TextureFormatInfo& tfi = s_textureFormat[_format];
		tfi.m_internalFmt = _internalFmt;
		tfi.m_fmt         = _fmt;
		tfi.m_type        = _type;
	}

	void initTestTexture(TextureFormat::Enum _format, bool srgb = false)
	{
		const TextureFormatInfo& tfi = s_textureFormat[_format];
		GLenum internalFmt = srgb
			? tfi.m_internalFmtSrgb
			: tfi.m_internalFmt
			;

		GLsizei size = (16*16*getBitsPerPixel(_format) )/8;
		void* data = alloca(size);

		if (isCompressed(_format) )
		{
			glCompressedTexImage2D(GL_TEXTURE_2D, 0, internalFmt, 16, 16, 0, size, data);
		}
		else
		{
			glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, 16, 16, 0, tfi.m_fmt, tfi.m_type, data);
		}
	}

	static bool isTextureFormatValid(TextureFormat::Enum _format, bool srgb = false)
	{
		const TextureFormatInfo& tfi = s_textureFormat[_format];
		GLenum internalFmt = srgb
			? tfi.m_internalFmtSrgb
			: tfi.m_internalFmt
			;
		if (!s_textureFormat[_format].m_supported
		||  GL_ZERO == internalFmt)
		{
			return false;
		}

		GLuint id;
		GL_CHECK(glGenTextures(1, &id) );
		GL_CHECK(glBindTexture(GL_TEXTURE_2D, id) );
		initTestTexture(_format);

		GLenum err = glGetError();
		BX_WARN(0 == err, "TextureFormat::%s is not supported (%x: %s).", getName(_format), err, glEnumName(err) );

		GL_CHECK(glDeleteTextures(1, &id) );

		return 0 == err;
	}

	static bool isImageFormatValid(TextureFormat::Enum _format)
	{
		if (GL_ZERO == s_imageFormat[_format])
		{
			return false;
		}

		GLuint id;
		GL_CHECK(glGenTextures(1, &id) );
		GL_CHECK(glBindTexture(GL_TEXTURE_2D, id) );
		glTexStorage2D(GL_TEXTURE_2D, 1, s_imageFormat[_format], 16, 16);
		GLenum err = glGetError();
		if (0 == err)
		{
			glBindImageTexture(0
				, id
				, 0
				, GL_FALSE
				, 0
				, GL_READ_WRITE
				, s_imageFormat[_format]
				);
			err = glGetError();
		}

		GL_CHECK(glDeleteTextures(1, &id) );

		return 0 == err;
	}

	static bool isFramebufferFormatValid(TextureFormat::Enum _format, bool srgb = false)
	{
		const TextureFormatInfo& tfi = s_textureFormat[_format];
		GLenum internalFmt = srgb
			? tfi.m_internalFmtSrgb
			: tfi.m_internalFmt
			;
		if (GL_ZERO == internalFmt
		||  !tfi.m_supported)
		{
			return false;
		}

		GLuint fbo;
		GL_CHECK(glGenFramebuffers(1, &fbo) );
		GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, fbo) );

		GLuint id;
		GL_CHECK(glGenTextures(1, &id) );
		GL_CHECK(glBindTexture(GL_TEXTURE_2D, id) );

		initTestTexture(_format);

		GLenum err = glGetError();

		GLenum attachment;
		if (isDepth(_format) )
		{
			const ImageBlockInfo& info = getBlockInfo(_format);
			if (0 == info.depthBits)
			{
				attachment = GL_STENCIL_ATTACHMENT;
			}
			else if (0 == info.stencilBits)
			{
				attachment = GL_DEPTH_ATTACHMENT;
			}
			else
			{
				attachment = GL_DEPTH_STENCIL_ATTACHMENT;
			}
		}
		else
		{
			attachment = GL_COLOR_ATTACHMENT0;
		}

		glFramebufferTexture2D(GL_FRAMEBUFFER
				, attachment
				, GL_TEXTURE_2D
				, id
				, 0
				);
		err = glGetError();

		if (0 == err)
		{
			err = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		}

		GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0) );
		GL_CHECK(glDeleteFramebuffers(1, &fbo) );

		GL_CHECK(glDeleteTextures(1, &id) );

		return GL_FRAMEBUFFER_COMPLETE == err;
	}

	static void getFilters(uint32_t _flags, bool _hasMips, GLenum& _magFilter, GLenum& _minFilter)
	{
		const uint32_t mag = (_flags&BGFX_TEXTURE_MAG_MASK)>>BGFX_TEXTURE_MAG_SHIFT;
		const uint32_t min = (_flags&BGFX_TEXTURE_MIN_MASK)>>BGFX_TEXTURE_MIN_SHIFT;
		const uint32_t mip = (_flags&BGFX_TEXTURE_MIP_MASK)>>BGFX_TEXTURE_MIP_SHIFT;
		_magFilter = s_textureFilterMag[mag];
		_minFilter = s_textureFilterMin[min][_hasMips ? mip+1 : 0];
	}

	struct RendererContextGL : public RendererContextI
	{
		RendererContextGL()
			: m_numWindows(1)
			, m_rtMsaa(false)
			, m_fbDiscard(BGFX_CLEAR_NONE)
			, m_capture(NULL)
			, m_captureSize(0)
			, m_maxAnisotropy(0.0f)
			, m_maxAnisotropyDefault(0.0f)
			, m_maxMsaa(0)
			, m_vao(0)
			, m_vaoSupport(false)
			, m_samplerObjectSupport(false)
			, m_shadowSamplersSupport(false)
			, m_programBinarySupport(false)
			, m_textureSwizzleSupport(false)
			, m_depthTextureSupport(false)
			, m_timerQuerySupport(false)
			, m_flip(false)
			, m_hash( (BX_PLATFORM_WINDOWS<<1) | BX_ARCH_64BIT)
			, m_backBufferFbo(0)
			, m_msaaBackBufferFbo(0)
			, m_ovrFbo(0)
		{
			memset(m_msaaBackBufferRbos, 0, sizeof(m_msaaBackBufferRbos) );
		}

		~RendererContextGL()
		{
		}

		void init()
		{
			m_renderdocdll = loadRenderDoc();

			m_fbh.idx = invalidHandle;
			memset(m_uniforms, 0, sizeof(m_uniforms) );
			memset(&m_resolution, 0, sizeof(m_resolution) );

			setRenderContextSize(BGFX_DEFAULT_WIDTH, BGFX_DEFAULT_HEIGHT);

			// Must be after context is initialized?!
			m_ovr.init();

			m_vendor      = getGLString(GL_VENDOR);
			m_renderer    = getGLString(GL_RENDERER);
			m_version     = getGLString(GL_VERSION);
			m_glslVersion = getGLString(GL_SHADING_LANGUAGE_VERSION);

			GLint numCmpFormats = 0;
			GL_CHECK(glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &numCmpFormats) );
			BX_TRACE("GL_NUM_COMPRESSED_TEXTURE_FORMATS %d", numCmpFormats);

			GLint* cmpFormat = NULL;

			if (0 < numCmpFormats)
			{
				numCmpFormats = numCmpFormats > 256 ? 256 : numCmpFormats;
				cmpFormat = (GLint*)alloca(sizeof(GLint)*numCmpFormats);
				GL_CHECK(glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, cmpFormat) );

				for (GLint ii = 0; ii < numCmpFormats; ++ii)
				{
					GLint internalFmt = cmpFormat[ii];
					uint32_t fmt = uint32_t(TextureFormat::Unknown);
					for (uint32_t jj = 0; jj < fmt; ++jj)
					{
						if (s_textureFormat[jj].m_internalFmt == (GLenum)internalFmt)
						{
							s_textureFormat[jj].m_supported = true;
							fmt = jj;
						}
					}

					BX_TRACE("  %3d: %8x %s", ii, internalFmt, getName( (TextureFormat::Enum)fmt) );
				}
			}

			if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
			{
#define GL_GET(_pname, _min) BX_TRACE("  " #_pname " %d (min: %d)", glGet(_pname), _min)
				BX_TRACE("Defaults:");
#if BGFX_CONFIG_RENDERER_OPENGL >= 41 || BGFX_CONFIG_RENDERER_OPENGLES
				GL_GET(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 16);
				GL_GET(GL_MAX_VERTEX_UNIFORM_VECTORS, 128);
				GL_GET(GL_MAX_VARYING_VECTORS, 8);
#else
				GL_GET(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, 16 * 4);
				GL_GET(GL_MAX_VERTEX_UNIFORM_COMPONENTS, 128 * 4);
				GL_GET(GL_MAX_VARYING_FLOATS, 8 * 4);
#endif // BGFX_CONFIG_RENDERER_OPENGL >= 41 || BGFX_CONFIG_RENDERER_OPENGLES
				GL_GET(GL_MAX_VERTEX_ATTRIBS, 8);
				GL_GET(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 8);
				GL_GET(GL_MAX_CUBE_MAP_TEXTURE_SIZE, 16);
				GL_GET(GL_MAX_TEXTURE_IMAGE_UNITS, 8);
				GL_GET(GL_MAX_TEXTURE_SIZE, 64);
				GL_GET(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 0);
				GL_GET(GL_MAX_RENDERBUFFER_SIZE, 1);
				GL_GET(GL_MAX_COLOR_ATTACHMENTS, 1);
				GL_GET(GL_MAX_DRAW_BUFFERS, 1);
#undef GL_GET

				BX_TRACE("      Vendor: %s", m_vendor);
				BX_TRACE("    Renderer: %s", m_renderer);
				BX_TRACE("     Version: %s", m_version);
				BX_TRACE("GLSL version: %s", m_glslVersion);
			}

			// Initial binary shader hash depends on driver version.
			m_hash = ( (BX_PLATFORM_WINDOWS<<1) | BX_ARCH_64BIT)
				^ (uint64_t(getGLStringHash(GL_VENDOR  ) )<<32)
				^ (uint64_t(getGLStringHash(GL_RENDERER) )<<0 )
				^ (uint64_t(getGLStringHash(GL_VERSION ) )<<16)
				;

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 31)
			&&  0    == strcmp(m_vendor,  "Imagination Technologies")
			&&  NULL != strstr(m_version, "(SDK 3.5@3510720)") )
			{
				// Skip initializing extensions that are broken in emulator.
				s_extension[Extension::ARB_program_interface_query     ].m_initialize =
				s_extension[Extension::ARB_shader_storage_buffer_object].m_initialize = false;
			}

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_USE_EXTENSIONS) )
			{
				const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
				glGetError(); // ignore error if glGetString returns NULL.
				if (NULL != extensions)
				{
					char name[1024];
					const char* pos = extensions;
					const char* end = extensions + strlen(extensions);
					uint32_t index = 0;
					while (pos < end)
					{
						uint32_t len;
						const char* space = strchr(pos, ' ');
						if (NULL != space)
						{
							len = bx::uint32_min(sizeof(name), (uint32_t)(space - pos) );
						}
						else
						{
							len = bx::uint32_min(sizeof(name), (uint32_t)strlen(pos) );
						}

						strncpy(name, pos, len);
						name[len] = '\0';

						bool supported = false;
						for (uint32_t ii = 0; ii < Extension::Count; ++ii)
						{
							Extension& extension = s_extension[ii];
							if (!extension.m_supported
							&&  extension.m_initialize)
							{
								const char* ext = name;
								if (0 == strncmp(ext, "GL_", 3) ) // skip GL_
								{
									ext += 3;
								}

								if (0 == strcmp(ext, extension.m_name) )
								{
									extension.m_supported = true;
									supported = true;
									break;
								}
							}
						}

						BX_TRACE("GL_EXTENSION %3d%s: %s", index, supported ? " (supported)" : "", name);
						BX_UNUSED(supported);

						pos += len+1;
						++index;
					}

					BX_TRACE("Supported extensions:");
					for (uint32_t ii = 0; ii < Extension::Count; ++ii)
					{
						if (s_extension[ii].m_supported)
						{
							BX_TRACE("\t%2d: %s", ii, s_extension[ii].m_name);
						}
					}
				}
			}

			// Allow all texture filters.
			memset(s_textureFilter, true, BX_COUNTOF(s_textureFilter) );
			for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
			{
				s_textureFormat[ii].m_supported = true
					&& TextureFormat::Unknown != ii
					&& TextureFormat::UnknownDepth != ii
					;
			}

			bool bc123Supported = 0
				|| s_extension[Extension::EXT_texture_compression_s3tc        ].m_supported
				|| s_extension[Extension::MOZ_WEBGL_compressed_texture_s3tc   ].m_supported
				|| s_extension[Extension::WEBGL_compressed_texture_s3tc       ].m_supported
				|| s_extension[Extension::WEBKIT_WEBGL_compressed_texture_s3tc].m_supported
				;
			s_textureFormat[TextureFormat::BC1].m_supported |= bc123Supported
				|| s_extension[Extension::ANGLE_texture_compression_dxt1].m_supported
				|| s_extension[Extension::EXT_texture_compression_dxt1  ].m_supported
				;

			if (!s_textureFormat[TextureFormat::BC1].m_supported
			&& ( s_textureFormat[TextureFormat::BC2].m_supported || s_textureFormat[TextureFormat::BC3].m_supported) )
			{
				// If RGBA_S3TC_DXT1 is not supported, maybe RGB_S3TC_DXT1 is?
				for (GLint ii = 0; ii < numCmpFormats; ++ii)
				{
					if (GL_COMPRESSED_RGB_S3TC_DXT1_EXT == cmpFormat[ii])
					{
						setTextureFormat(TextureFormat::BC1, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
						s_textureFormat[TextureFormat::BC1].m_supported   = true;
						break;
					}
				}
			}

			s_textureFormat[TextureFormat::BC2].m_supported |= bc123Supported
				|| s_extension[Extension::ANGLE_texture_compression_dxt3   ].m_supported
				|| s_extension[Extension::CHROMIUM_texture_compression_dxt3].m_supported
				;

			s_textureFormat[TextureFormat::BC3].m_supported |= bc123Supported
				|| s_extension[Extension::ANGLE_texture_compression_dxt5   ].m_supported
				|| s_extension[Extension::CHROMIUM_texture_compression_dxt5].m_supported
				;

			if (s_extension[Extension::EXT_texture_compression_latc].m_supported)
			{
				setTextureFormat(TextureFormat::BC4, GL_COMPRESSED_LUMINANCE_LATC1_EXT,       GL_COMPRESSED_LUMINANCE_LATC1_EXT);
				setTextureFormat(TextureFormat::BC5, GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT, GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT);
			}

			if (s_extension[Extension::ARB_texture_compression_rgtc].m_supported
			||  s_extension[Extension::EXT_texture_compression_rgtc].m_supported)
			{
				setTextureFormat(TextureFormat::BC4, GL_COMPRESSED_RED_RGTC1, GL_COMPRESSED_RED_RGTC1);
				setTextureFormat(TextureFormat::BC5, GL_COMPRESSED_RG_RGTC2,  GL_COMPRESSED_RG_RGTC2);
			}

			bool etc1Supported = 0
				|| s_extension[Extension::OES_compressed_ETC1_RGB8_texture].m_supported
				|| s_extension[Extension::WEBGL_compressed_texture_etc1   ].m_supported
				;
			s_textureFormat[TextureFormat::ETC1].m_supported |= etc1Supported;

			bool etc2Supported = !!(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::ARB_ES3_compatibility].m_supported
				;
			s_textureFormat[TextureFormat::ETC2  ].m_supported |= etc2Supported;
			s_textureFormat[TextureFormat::ETC2A ].m_supported |= etc2Supported;
			s_textureFormat[TextureFormat::ETC2A1].m_supported |= etc2Supported;

			if (!s_textureFormat[TextureFormat::ETC1].m_supported
			&&   s_textureFormat[TextureFormat::ETC2].m_supported)
			{
				// When ETC2 is supported override ETC1 texture format settings.
				s_textureFormat[TextureFormat::ETC1].m_internalFmt = GL_COMPRESSED_RGB8_ETC2;
				s_textureFormat[TextureFormat::ETC1].m_fmt         = GL_COMPRESSED_RGB8_ETC2;
				s_textureFormat[TextureFormat::ETC1].m_supported   = true;
			}

			bool ptc1Supported = 0
				|| s_extension[Extension::IMG_texture_compression_pvrtc ].m_supported
				|| s_extension[Extension::WEBGL_compressed_texture_pvrtc].m_supported
				;
			s_textureFormat[TextureFormat::PTC12 ].m_supported |= ptc1Supported;
			s_textureFormat[TextureFormat::PTC14 ].m_supported |= ptc1Supported;
			s_textureFormat[TextureFormat::PTC12A].m_supported |= ptc1Supported;
			s_textureFormat[TextureFormat::PTC14A].m_supported |= ptc1Supported;

			bool ptc2Supported = s_extension[Extension::IMG_texture_compression_pvrtc2].m_supported;
			s_textureFormat[TextureFormat::PTC22].m_supported |= ptc2Supported;
			s_textureFormat[TextureFormat::PTC24].m_supported |= ptc2Supported;

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES) )
			{
				setTextureFormat(TextureFormat::D32, GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);

				if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES < 30) )
				{
					setTextureFormat(TextureFormat::RGBA16F, GL_RGBA, GL_RGBA, GL_HALF_FLOAT);
					// internalFormat and format must match:
					// https://www.khronos.org/opengles/sdk/docs/man/xhtml/glTexImage2D.xml
					setTextureFormat(TextureFormat::RGBA8,  GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE);
					setTextureFormat(TextureFormat::R5G6B5, GL_RGB,  GL_RGB,  GL_UNSIGNED_SHORT_5_6_5);
					setTextureFormat(TextureFormat::RGBA4,  GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4);
					setTextureFormat(TextureFormat::RGB5A1, GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1);

					if (s_extension[Extension::OES_texture_half_float].m_supported
					||  s_extension[Extension::OES_texture_float     ].m_supported)
					{
						// https://www.khronos.org/registry/gles/extensions/OES/OES_texture_float.txt
						// When half/float is available via extensions texture will be marked as
						// incomplete if it uses anything other than nearest filter.
						const bool linear16F = s_extension[Extension::OES_texture_half_float_linear].m_supported;
						const bool linear32F = s_extension[Extension::OES_texture_float_linear     ].m_supported;

						s_textureFilter[TextureFormat::R16F]    = linear16F;
						s_textureFilter[TextureFormat::RG16F]   = linear16F;
						s_textureFilter[TextureFormat::RGBA16F] = linear16F;
						s_textureFilter[TextureFormat::R32F]    = linear32F;
						s_textureFilter[TextureFormat::RG32F]   = linear32F;
						s_textureFilter[TextureFormat::RGBA32F] = linear32F;
					}

					if (BX_ENABLED(BX_PLATFORM_IOS) )
					{
						setTextureFormat(TextureFormat::D16,   GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT);
						setTextureFormat(TextureFormat::D24S8, GL_DEPTH_STENCIL,   GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8);
					}
				}
			}

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			||  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
			{
				setTextureFormat(TextureFormat::R16I,    GL_R16I,     GL_RED_INTEGER,  GL_SHORT);
				setTextureFormat(TextureFormat::R16U,    GL_R16UI,    GL_RED_INTEGER,  GL_UNSIGNED_SHORT);
//				setTextureFormat(TextureFormat::RG16,    GL_RG16UI,   GL_RG_INTEGER,   GL_UNSIGNED_SHORT);
//				setTextureFormat(TextureFormat::RGBA16,  GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT);
				setTextureFormat(TextureFormat::R32U,    GL_R32UI,    GL_RED_INTEGER,  GL_UNSIGNED_INT);
				setTextureFormat(TextureFormat::RG32U,   GL_RG32UI,   GL_RG_INTEGER,   GL_UNSIGNED_INT);
				setTextureFormat(TextureFormat::RGBA32U, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
			}

			if (s_extension[Extension::EXT_texture_format_BGRA8888  ].m_supported
			||  s_extension[Extension::EXT_bgra                     ].m_supported
			||  s_extension[Extension::IMG_texture_format_BGRA8888  ].m_supported
			||  s_extension[Extension::APPLE_texture_format_BGRA8888].m_supported)
			{
				if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) )
				{
					m_readPixelsFmt = GL_BGRA;
				}

				// Mixing GLES and GL extensions here. OpenGL EXT_bgra and
				// APPLE_texture_format_BGRA8888 wants
				// format to be BGRA but internal format to stay RGBA, but
				// EXT_texture_format_BGRA8888 wants both format and internal
				// format to be BGRA.
				//
				// Reference:
				// https://www.khronos.org/registry/gles/extensions/EXT/EXT_texture_format_BGRA8888.txt
				// https://www.opengl.org/registry/specs/EXT/bgra.txt
				// https://www.khronos.org/registry/gles/extensions/APPLE/APPLE_texture_format_BGRA8888.txt
				if (!s_extension[Extension::EXT_bgra                     ].m_supported
				&&  !s_extension[Extension::APPLE_texture_format_BGRA8888].m_supported)
				{
					s_textureFormat[TextureFormat::BGRA8].m_internalFmt = GL_BGRA;
				}

				if (!isTextureFormatValid(TextureFormat::BGRA8) )
				{
					// Revert back to RGBA if texture can't be created.
					setTextureFormat(TextureFormat::BGRA8, GL_RGBA, GL_BGRA, GL_UNSIGNED_BYTE);
				}
			}

			if (BX_ENABLED(BX_PLATFORM_EMSCRIPTEN)
			||  !isTextureFormatValid(TextureFormat::R8) )
			{
				// GL core has to use GL_R8 Issue#208, GLES2 has to use GL_LUMINANCE issue#226
				s_textureFormat[TextureFormat::R8].m_internalFmt = GL_LUMINANCE;
				s_textureFormat[TextureFormat::R8].m_fmt         = GL_LUMINANCE;
			}

			for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
			{
				if (TextureFormat::Unknown != ii
				&&  TextureFormat::UnknownDepth != ii)
				{
					s_textureFormat[ii].m_supported = isTextureFormatValid(TextureFormat::Enum(ii) );
				}
			}

			if (BX_ENABLED(0) )
			{
				// Disable all compressed texture formats. For testing only.
				for (uint32_t ii = 0; ii < TextureFormat::Unknown; ++ii)
				{
					s_textureFormat[ii].m_supported = false;
				}
			}

			const bool computeSupport = false
				|| !!(BGFX_CONFIG_RENDERER_OPENGLES >= 31)
				|| s_extension[Extension::ARB_compute_shader].m_supported
				;

			for (uint32_t ii = 0; ii < TextureFormat::Count; ++ii)
			{
				uint8_t supported = 0;
				supported |= s_textureFormat[ii].m_supported
					? BGFX_CAPS_FORMAT_TEXTURE_COLOR
					: BGFX_CAPS_FORMAT_TEXTURE_NONE
					;

				supported |= isTextureFormatValid(TextureFormat::Enum(ii), true)
					? BGFX_CAPS_FORMAT_TEXTURE_COLOR_SRGB
					: BGFX_CAPS_FORMAT_TEXTURE_NONE
					;

				supported |= computeSupport
					&& isImageFormatValid(TextureFormat::Enum(ii) )
					? BGFX_CAPS_FORMAT_TEXTURE_IMAGE
					: BGFX_CAPS_FORMAT_TEXTURE_NONE
					;

				supported |= isFramebufferFormatValid(TextureFormat::Enum(ii) )
					? BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER
					: BGFX_CAPS_FORMAT_TEXTURE_NONE
					;

				g_caps.formats[ii] = supported;
			}

			g_caps.supported |= !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::OES_texture_3D].m_supported
				? BGFX_CAPS_TEXTURE_3D
				: 0
				;
			g_caps.supported |= !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::EXT_shadow_samplers].m_supported
				? BGFX_CAPS_TEXTURE_COMPARE_ALL
				: 0
				;
			g_caps.supported |= !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::OES_vertex_half_float].m_supported
				? BGFX_CAPS_VERTEX_ATTRIB_HALF
				: 0
				;
			g_caps.supported |= false
				|| s_extension[Extension::ARB_vertex_type_2_10_10_10_rev].m_supported
				|| s_extension[Extension::OES_vertex_type_10_10_10_2].m_supported
				? BGFX_CAPS_VERTEX_ATTRIB_UINT10
				: 0
				;
			g_caps.supported |= !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::EXT_frag_depth].m_supported
				? BGFX_CAPS_FRAGMENT_DEPTH
				: 0
				;
			g_caps.supported |= s_extension[Extension::ARB_draw_buffers_blend].m_supported
				? BGFX_CAPS_BLEND_INDEPENDENT
				: 0
				;
			g_caps.supported |= s_extension[Extension::INTEL_fragment_shader_ordering].m_supported
				? BGFX_CAPS_FRAGMENT_ORDERING
				: 0
				;
			g_caps.supported |= !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::OES_element_index_uint].m_supported
				? BGFX_CAPS_INDEX32
				: 0
				;

			const bool drawIndirectSupported = false
				|| s_extension[Extension::AMD_multi_draw_indirect].m_supported
				|| s_extension[Extension::ARB_draw_indirect      ].m_supported
				|| s_extension[Extension::ARB_multi_draw_indirect].m_supported
				|| s_extension[Extension::EXT_multi_draw_indirect].m_supported
				;

			if (drawIndirectSupported)
			{
				if (NULL == glMultiDrawArraysIndirect
				||  NULL == glMultiDrawElementsIndirect)
				{
					glMultiDrawArraysIndirect   = stubMultiDrawArraysIndirect;
					glMultiDrawElementsIndirect = stubMultiDrawElementsIndirect;
				}
			}

			g_caps.supported |= drawIndirectSupported
				? BGFX_CAPS_DRAW_INDIRECT
				: 0
				;

			g_caps.maxTextureSize = uint16_t(glGet(GL_MAX_TEXTURE_SIZE) );

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			||  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
			||  s_extension[Extension::EXT_draw_buffers  ].m_supported
			||  s_extension[Extension::WEBGL_draw_buffers].m_supported)
			{
				g_caps.maxFBAttachments = uint8_t(bx::uint32_min(glGet(GL_MAX_DRAW_BUFFERS)
						, BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS)
						);
			}

			m_vaoSupport = !!(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::ARB_vertex_array_object].m_supported
				|| s_extension[Extension::OES_vertex_array_object].m_supported
				;

			if (BX_ENABLED(BX_PLATFORM_NACL) )
			{
				m_vaoSupport &= true
					&& NULL != glGenVertexArrays
					&& NULL != glDeleteVertexArrays
					&& NULL != glBindVertexArray
					;
			}

			if (m_vaoSupport)
			{
				GL_CHECK(glGenVertexArrays(1, &m_vao) );
			}

			m_samplerObjectSupport = !!(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::ARB_sampler_objects].m_supported
				;

			m_shadowSamplersSupport = !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::EXT_shadow_samplers].m_supported
				;

			m_programBinarySupport = !!(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::ARB_get_program_binary].m_supported
				|| s_extension[Extension::OES_get_program_binary].m_supported
				|| s_extension[Extension::IMG_shader_binary     ].m_supported
				;

			m_textureSwizzleSupport = false
				|| s_extension[Extension::ARB_texture_swizzle].m_supported
				|| s_extension[Extension::EXT_texture_swizzle].m_supported
				;

			m_depthTextureSupport = !!(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
				|| s_extension[Extension::ANGLE_depth_texture       ].m_supported
				|| s_extension[Extension::CHROMIUM_depth_texture    ].m_supported
				|| s_extension[Extension::GOOGLE_depth_texture      ].m_supported
				|| s_extension[Extension::OES_depth_texture         ].m_supported
				|| s_extension[Extension::MOZ_WEBGL_depth_texture   ].m_supported
				|| s_extension[Extension::WEBGL_depth_texture       ].m_supported
				|| s_extension[Extension::WEBKIT_WEBGL_depth_texture].m_supported
				;

			m_timerQuerySupport = false
				|| s_extension[Extension::ANGLE_timer_query       ].m_supported
				|| s_extension[Extension::ARB_timer_query         ].m_supported
				|| s_extension[Extension::EXT_disjoint_timer_query].m_supported
				|| s_extension[Extension::EXT_timer_query         ].m_supported
				;

			m_timerQuerySupport &= true
				&& NULL != glGetQueryObjectiv
				&& NULL != glGetQueryObjectui64v
				;

			g_caps.supported |= m_depthTextureSupport
				? BGFX_CAPS_TEXTURE_COMPARE_LEQUAL
				: 0
				;

			g_caps.supported |= computeSupport
				? BGFX_CAPS_COMPUTE
				: 0
				;

			g_caps.supported |= m_glctx.getCaps();

			if (s_extension[Extension::EXT_texture_filter_anisotropic].m_supported)
			{
				GL_CHECK(glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m_maxAnisotropyDefault) );
			}

			if (s_extension[Extension::ARB_texture_multisample].m_supported
			||  s_extension[Extension::ANGLE_framebuffer_multisample].m_supported)
			{
				GL_CHECK(glGetIntegerv(GL_MAX_SAMPLES, &m_maxMsaa) );
			}

			if (s_extension[Extension::OES_read_format].m_supported
			&& (s_extension[Extension::IMG_read_format].m_supported	|| s_extension[Extension::EXT_read_format_bgra].m_supported) )
			{
				m_readPixelsFmt = GL_BGRA;
			}
			else
			{
				m_readPixelsFmt = GL_RGBA;
			}

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
			{
				g_caps.supported |= BGFX_CAPS_INSTANCING;
			}
			else
			{
				if (!BX_ENABLED(BX_PLATFORM_IOS) )
				{
					if (s_extension[Extension::ARB_instanced_arrays].m_supported
					||  s_extension[Extension::ANGLE_instanced_arrays].m_supported)
					{
						if (NULL != glVertexAttribDivisor
						&&  NULL != glDrawArraysInstanced
						&&  NULL != glDrawElementsInstanced)
						{
							g_caps.supported |= BGFX_CAPS_INSTANCING;
						}
					}
				}

				if (0 == (g_caps.supported & BGFX_CAPS_INSTANCING) )
				{
					glVertexAttribDivisor   = stubVertexAttribDivisor;
					glDrawArraysInstanced   = stubDrawArraysInstanced;
					glDrawElementsInstanced = stubDrawElementsInstanced;
				}
			}

			if (s_extension[Extension::ARB_debug_output].m_supported
			||  s_extension[Extension::KHR_debug].m_supported)
			{
				if (NULL != glDebugMessageControl
				&&  NULL != glDebugMessageInsert
				&&  NULL != glDebugMessageCallback
				&&  NULL != glGetDebugMessageLog)
				{
					GL_CHECK(glDebugMessageCallback(debugProcCb, NULL) );
					GL_CHECK(glDebugMessageControl(GL_DONT_CARE
							, GL_DONT_CARE
							, GL_DEBUG_SEVERITY_MEDIUM
							, 0
							, NULL
							, GL_TRUE
							) );
				}
			}

			if (s_extension[Extension::ARB_seamless_cube_map].m_supported)
			{
				GL_CHECK(glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS) );
			}

			if (s_extension[Extension::ARB_depth_clamp].m_supported)
			{
				GL_CHECK(glEnable(GL_DEPTH_CLAMP) );
			}

			if (NULL == glFrameTerminatorGREMEDY
			||  !s_extension[Extension::GREMEDY_frame_terminator].m_supported)
			{
				glFrameTerminatorGREMEDY = stubFrameTerminatorGREMEDY;
			}

			if (NULL == glInsertEventMarker
			||  !s_extension[Extension::EXT_debug_marker].m_supported)
			{
				glInsertEventMarker = (NULL != glStringMarkerGREMEDY && s_extension[Extension::GREMEDY_string_marker].m_supported)
					? stubInsertEventMarkerGREMEDY
					: stubInsertEventMarker
					;
			}

			setGraphicsDebuggerPresent(s_extension[Extension::EXT_debug_tool].m_supported);

			if (NULL == glObjectLabel)
			{
				glObjectLabel = stubObjectLabel;
			}

			if (NULL == glInvalidateFramebuffer)
			{
				glInvalidateFramebuffer = stubInvalidateFramebuffer;
			}

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			&&  m_timerQuerySupport)
			{
				m_gpuTimer.create();
			}

			// Init reserved part of view name.
			for (uint32_t ii = 0; ii < BGFX_CONFIG_MAX_VIEWS; ++ii)
			{
				bx::snprintf(s_viewName[ii], BGFX_CONFIG_MAX_VIEW_NAME_RESERVED+1, "%3d   ", ii);
			}

			ovrPostReset();
		}

		void shutdown()
		{
			ovrPreReset();
			m_ovr.shutdown();

			if (m_vaoSupport)
			{
				GL_CHECK(glBindVertexArray(0) );
				GL_CHECK(glDeleteVertexArrays(1, &m_vao) );
				m_vao = 0;
			}

			captureFinish();

			invalidateCache();

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			&&  m_timerQuerySupport)
			{
				m_gpuTimer.destroy();
			}

			destroyMsaaFbo();
			m_glctx.destroy();

			m_flip = false;

			unloadRenderDoc(m_renderdocdll);
		}

		RendererType::Enum getRendererType() const BX_OVERRIDE
		{
			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) )
			{
				return RendererType::OpenGL;
			}

			return RendererType::OpenGLES;
		}

		const char* getRendererName() const BX_OVERRIDE
		{
			return BGFX_RENDERER_OPENGL_NAME;
		}

		void flip(HMD& _hmd)
		{
			if (m_flip)
			{
				for (uint32_t ii = 1, num = m_numWindows; ii < num; ++ii)
				{
					m_glctx.swap(m_frameBuffers[m_windows[ii].idx].m_swapChain);
				}

				if (!m_ovr.swap(_hmd) )
				{
					m_glctx.swap();
				}
			}
		}

		void createIndexBuffer(IndexBufferHandle _handle, Memory* _mem, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_mem->size, _mem->data, _flags);
		}

		void destroyIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createVertexDecl(VertexDeclHandle _handle, const VertexDecl& _decl) BX_OVERRIDE
		{
			VertexDecl& decl = m_vertexDecls[_handle.idx];
			memcpy(&decl, &_decl, sizeof(VertexDecl) );
			dump(decl);
		}

		void destroyVertexDecl(VertexDeclHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createVertexBuffer(VertexBufferHandle _handle, Memory* _mem, VertexDeclHandle _declHandle, uint16_t _flags) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].create(_mem->size, _mem->data, _declHandle, _flags);
		}

		void destroyVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].create(_size, NULL, _flags);
		}

		void updateDynamicIndexBuffer(IndexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].update(_offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicIndexBuffer(IndexBufferHandle _handle) BX_OVERRIDE
		{
			m_indexBuffers[_handle.idx].destroy();
		}

		void createDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _size, uint16_t _flags) BX_OVERRIDE
		{
			VertexDeclHandle decl = BGFX_INVALID_HANDLE;
			m_vertexBuffers[_handle.idx].create(_size, NULL, decl, _flags);
		}

		void updateDynamicVertexBuffer(VertexBufferHandle _handle, uint32_t _offset, uint32_t _size, Memory* _mem) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].update(_offset, bx::uint32_min(_size, _mem->size), _mem->data);
		}

		void destroyDynamicVertexBuffer(VertexBufferHandle _handle) BX_OVERRIDE
		{
			m_vertexBuffers[_handle.idx].destroy();
		}

		void createShader(ShaderHandle _handle, Memory* _mem) BX_OVERRIDE
		{
			m_shaders[_handle.idx].create(_mem);
		}

		void destroyShader(ShaderHandle _handle) BX_OVERRIDE
		{
			m_shaders[_handle.idx].destroy();
		}

		void createProgram(ProgramHandle _handle, ShaderHandle _vsh, ShaderHandle _fsh) BX_OVERRIDE
		{
			ShaderGL dummyFragmentShader;
			m_program[_handle.idx].create(m_shaders[_vsh.idx], isValid(_fsh) ? m_shaders[_fsh.idx] : dummyFragmentShader);
		}

		void destroyProgram(ProgramHandle _handle) BX_OVERRIDE
		{
			m_program[_handle.idx].destroy();
		}

		void createTexture(TextureHandle _handle, Memory* _mem, uint32_t _flags, uint8_t _skip) BX_OVERRIDE
		{
			m_textures[_handle.idx].create(_mem, _flags, _skip);
		}

		void updateTextureBegin(TextureHandle /*_handle*/, uint8_t /*_side*/, uint8_t /*_mip*/) BX_OVERRIDE
		{
		}

		void updateTexture(TextureHandle _handle, uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem) BX_OVERRIDE
		{
			m_textures[_handle.idx].update(_side, _mip, _rect, _z, _depth, _pitch, _mem);
		}

		void updateTextureEnd() BX_OVERRIDE
		{
		}

		void resizeTexture(TextureHandle _handle, uint16_t _width, uint16_t _height) BX_OVERRIDE
		{
			TextureGL& texture = m_textures[_handle.idx];

			uint32_t size = sizeof(uint32_t) + sizeof(TextureCreate);
			const Memory* mem = alloc(size);

			bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
			uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
			bx::write(&writer, magic);

			TextureCreate tc;
			tc.m_flags   = texture.m_flags;
			tc.m_width   = _width;
			tc.m_height  = _height;
			tc.m_sides   = 0;
			tc.m_depth   = 0;
			tc.m_numMips = 1;
			tc.m_format  = texture.m_requestedFormat;
			tc.m_cubeMap = false;
			tc.m_mem     = NULL;
			bx::write(&writer, tc);

			texture.destroy();
			texture.create(mem, tc.m_flags, 0);

			release(mem);
		}

		void destroyTexture(TextureHandle _handle) BX_OVERRIDE
		{
			m_textures[_handle.idx].destroy();
		}

		void createFrameBuffer(FrameBufferHandle _handle, uint8_t _num, const TextureHandle* _textureHandles) BX_OVERRIDE
		{
			m_frameBuffers[_handle.idx].create(_num, _textureHandles);
		}

		void createFrameBuffer(FrameBufferHandle _handle, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat) BX_OVERRIDE
		{
			uint16_t denseIdx = m_numWindows++;
			m_windows[denseIdx] = _handle;
			m_frameBuffers[_handle.idx].create(denseIdx, _nwh, _width, _height, _depthFormat);
		}

		void destroyFrameBuffer(FrameBufferHandle _handle) BX_OVERRIDE
		{
			uint16_t denseIdx = m_frameBuffers[_handle.idx].destroy();
			if (UINT16_MAX != denseIdx)
			{
				--m_numWindows;
				if (m_numWindows > 1)
				{
					FrameBufferHandle handle = m_windows[m_numWindows];
					m_windows[denseIdx] = handle;
					m_frameBuffers[handle.idx].m_denseIdx = denseIdx;
				}
			}
		}

		void createUniform(UniformHandle _handle, UniformType::Enum _type, uint16_t _num, const char* _name) BX_OVERRIDE
		{
			if (NULL != m_uniforms[_handle.idx])
			{
				BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			}

			uint32_t size = g_uniformTypeSize[_type]*_num;
			void* data = BX_ALLOC(g_allocator, size);
			memset(data, 0, size);
			m_uniforms[_handle.idx] = data;
			m_uniformReg.add(_handle, _name, m_uniforms[_handle.idx]);
		}

		void destroyUniform(UniformHandle _handle) BX_OVERRIDE
		{
			BX_FREE(g_allocator, m_uniforms[_handle.idx]);
			m_uniforms[_handle.idx] = NULL;
		}

		void saveScreenShot(const char* _filePath) BX_OVERRIDE
		{
			uint32_t length = m_resolution.m_width*m_resolution.m_height*4;
			uint8_t* data = (uint8_t*)BX_ALLOC(g_allocator, length);

			uint32_t width  = m_resolution.m_width;
			uint32_t height = m_resolution.m_height;

			GL_CHECK(glReadPixels(0
				, 0
				, width
				, height
				, m_readPixelsFmt
				, GL_UNSIGNED_BYTE
				, data
				) );

			if (GL_RGBA == m_readPixelsFmt)
			{
				imageSwizzleBgra8(width, height, width*4, data, data);
			}

			g_callback->screenShot(_filePath
				, width
				, height
				, width*4
				, data
				, length
				, true
				);
			BX_FREE(g_allocator, data);
		}

		void updateViewName(uint8_t _id, const char* _name) BX_OVERRIDE
		{
			if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
			{
				bx::strlcpy(&s_viewName[_id][BGFX_CONFIG_MAX_VIEW_NAME_RESERVED]
					, _name
					, BX_COUNTOF(s_viewName[0])-BGFX_CONFIG_MAX_VIEW_NAME_RESERVED
					);
			}
		}

		void updateUniform(uint16_t _loc, const void* _data, uint32_t _size) BX_OVERRIDE
		{
			memcpy(m_uniforms[_loc], _data, _size);
		}

		void setMarker(const char* _marker, uint32_t _size) BX_OVERRIDE
		{
			GL_CHECK(glInsertEventMarker(_size, _marker) );
		}

		void submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter) BX_OVERRIDE;

		void blitSetup(TextVideoMemBlitter& _blitter) BX_OVERRIDE
		{
			if (0 != m_vao)
			{
				GL_CHECK(glBindVertexArray(m_vao) );
			}

			uint32_t width  = m_resolution.m_width;
			uint32_t height = m_resolution.m_height;
			if (m_ovr.isEnabled() )
			{
				m_ovr.getSize(width, height);
			}

			GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_backBufferFbo) );
			GL_CHECK(glViewport(0, 0, width, height) );

			GL_CHECK(glDisable(GL_SCISSOR_TEST) );
			GL_CHECK(glDisable(GL_STENCIL_TEST) );
			GL_CHECK(glDisable(GL_DEPTH_TEST) );
			GL_CHECK(glDepthFunc(GL_ALWAYS) );
			GL_CHECK(glDisable(GL_CULL_FACE) );
			GL_CHECK(glDisable(GL_BLEND) );
			GL_CHECK(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE) );

			ProgramGL& program = m_program[_blitter.m_program.idx];
			GL_CHECK(glUseProgram(program.m_id) );
			GL_CHECK(glUniform1i(program.m_sampler[0], 0) );

			float proj[16];
			bx::mtxOrtho(proj, 0.0f, (float)width, (float)height, 0.0f, 0.0f, 1000.0f);

			GL_CHECK(glUniformMatrix4fv(program.m_predefined[0].m_loc
				, 1
				, GL_FALSE
				, proj
				) );

			GL_CHECK(glActiveTexture(GL_TEXTURE0) );
			GL_CHECK(glBindTexture(GL_TEXTURE_2D, m_textures[_blitter.m_texture.idx].m_id) );
		}

		void blitRender(TextVideoMemBlitter& _blitter, uint32_t _numIndices) BX_OVERRIDE
		{
			const uint32_t numVertices = _numIndices*4/6;
			if (0 < numVertices)
			{
				m_indexBuffers[_blitter.m_ib->handle.idx].update(0, _numIndices*2, _blitter.m_ib->data);
				m_vertexBuffers[_blitter.m_vb->handle.idx].update(0, numVertices*_blitter.m_decl.m_stride, _blitter.m_vb->data);

				VertexBufferGL& vb = m_vertexBuffers[_blitter.m_vb->handle.idx];
				GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vb.m_id) );

				IndexBufferGL& ib = m_indexBuffers[_blitter.m_ib->handle.idx];
				GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib.m_id) );

				ProgramGL& program = m_program[_blitter.m_program.idx];
				program.bindAttributes(_blitter.m_decl, 0);

				GL_CHECK(glDrawElements(GL_TRIANGLES
					, _numIndices
					, GL_UNSIGNED_SHORT
					, (void*)0
					) );
			}
		}

		void updateResolution(const Resolution& _resolution)
		{
			bool recenter   = !!(_resolution.m_flags & BGFX_RESET_HMD_RECENTER);
			m_maxAnisotropy = !!(_resolution.m_flags & BGFX_RESET_MAXANISOTROPY)
				? m_maxAnisotropyDefault
				: 0.0f
				;
			uint32_t flags = _resolution.m_flags & ~(BGFX_RESET_HMD_RECENTER | BGFX_RESET_MAXANISOTROPY);

			if (m_resolution.m_width  != _resolution.m_width
			||  m_resolution.m_height != _resolution.m_height
			||  m_resolution.m_flags  != flags)
			{
				m_textVideoMem.resize(false, _resolution.m_width, _resolution.m_height);
				m_textVideoMem.clear();

				m_resolution = _resolution;
				m_resolution.m_flags = flags;

				if ( (flags & BGFX_RESET_HMD)
				&&  m_ovr.isInitialized() )
				{
					flags &= ~BGFX_RESET_MSAA_MASK;
				}

				setRenderContextSize(m_resolution.m_width
						, m_resolution.m_height
						, flags
						);
				updateCapture();

				for (uint32_t ii = 0; ii < BX_COUNTOF(m_frameBuffers); ++ii)
				{
					m_frameBuffers[ii].postReset();
				}

				ovrPreReset();
				ovrPostReset();
			}

			if (recenter)
			{
				m_ovr.recenter();
			}
		}

		void setShaderUniform4f(uint8_t /*_flags*/, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			GL_CHECK(glUniform4fv(_regIndex
				, _numRegs
				, (const GLfloat*)_val
				) );
		}

		void setShaderUniform4x4f(uint8_t /*_flags*/, uint32_t _regIndex, const void* _val, uint32_t _numRegs)
		{
			GL_CHECK(glUniformMatrix4fv(_regIndex
				, _numRegs
				, GL_FALSE
				, (const GLfloat*)_val
				) );
		}

		uint32_t setFrameBuffer(FrameBufferHandle _fbh, uint32_t _height, uint16_t _discard = BGFX_CLEAR_NONE, bool _msaa = true)
		{
			if (isValid(m_fbh)
			&&  m_fbh.idx != _fbh.idx
			&& (BGFX_CLEAR_NONE != m_fbDiscard || m_rtMsaa) )
			{
				FrameBufferGL& frameBuffer = m_frameBuffers[m_fbh.idx];
				if (m_rtMsaa)
				{
					frameBuffer.resolve();
				}

				if (BGFX_CLEAR_NONE != m_fbDiscard)
				{
					frameBuffer.discard(m_fbDiscard);
				}

				m_fbDiscard = BGFX_CLEAR_NONE;
			}

			m_glctx.makeCurrent(NULL);

			if (!isValid(_fbh) )
			{
				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_msaaBackBufferFbo) );
			}
			else
			{
				FrameBufferGL& frameBuffer = m_frameBuffers[_fbh.idx];
				_height = frameBuffer.m_height;
				if (UINT16_MAX != frameBuffer.m_denseIdx)
				{
					m_glctx.makeCurrent(frameBuffer.m_swapChain);
					GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0) );
				}
				else
				{
					m_glctx.makeCurrent(NULL);
					GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer.m_fbo[0]) );
				}
			}

			m_fbh       = _fbh;
			m_fbDiscard = _discard;
			m_rtMsaa    = _msaa;

			return _height;
		}

		uint32_t getNumRt() const
		{
			if (isValid(m_fbh) )
			{
				const FrameBufferGL& frameBuffer = m_frameBuffers[m_fbh.idx];
				return frameBuffer.m_num;
			}

			return 1;
		}

		void createMsaaFbo(uint32_t _width, uint32_t _height, uint32_t _msaa)
		{
			if (0 == m_msaaBackBufferFbo // iOS
			&&  1 < _msaa)
			{
				GL_CHECK(glGenFramebuffers(1, &m_msaaBackBufferFbo) );
				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_msaaBackBufferFbo) );
				GL_CHECK(glGenRenderbuffers(BX_COUNTOF(m_msaaBackBufferRbos), m_msaaBackBufferRbos) );
				GL_CHECK(glBindRenderbuffer(GL_RENDERBUFFER, m_msaaBackBufferRbos[0]) );
				GL_CHECK(glRenderbufferStorageMultisample(GL_RENDERBUFFER, _msaa, GL_RGBA8, _width, _height) );
				GL_CHECK(glBindRenderbuffer(GL_RENDERBUFFER, m_msaaBackBufferRbos[1]) );
				GL_CHECK(glRenderbufferStorageMultisample(GL_RENDERBUFFER, _msaa, GL_DEPTH24_STENCIL8, _width, _height) );
				GL_CHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_msaaBackBufferRbos[0]) );

				GLenum attachment = BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) || BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
					? GL_DEPTH_STENCIL_ATTACHMENT
					: GL_DEPTH_ATTACHMENT
					;
				GL_CHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, m_msaaBackBufferRbos[1]) );

				BX_CHECK(GL_FRAMEBUFFER_COMPLETE ==  glCheckFramebufferStatus(GL_FRAMEBUFFER)
					, "glCheckFramebufferStatus failed 0x%08x"
					, glCheckFramebufferStatus(GL_FRAMEBUFFER)
					);

				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_msaaBackBufferFbo) );
			}
		}

		void destroyMsaaFbo()
		{
			if (m_backBufferFbo != m_msaaBackBufferFbo // iOS
			&&  0 != m_msaaBackBufferFbo)
			{
				GL_CHECK(glDeleteFramebuffers(1, &m_msaaBackBufferFbo) );
				m_msaaBackBufferFbo = 0;

				if (0 != m_msaaBackBufferRbos[0])
				{
					GL_CHECK(glDeleteRenderbuffers(BX_COUNTOF(m_msaaBackBufferRbos), m_msaaBackBufferRbos) );
					m_msaaBackBufferRbos[0] = 0;
					m_msaaBackBufferRbos[1] = 0;
				}
			}
		}

		void blitMsaaFbo()
		{
			if (m_backBufferFbo != m_msaaBackBufferFbo // iOS
			&&  0 != m_msaaBackBufferFbo)
			{
				GL_CHECK(glDisable(GL_SCISSOR_TEST) );
				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_backBufferFbo) );
				GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaBackBufferFbo) );
				GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0) );
				uint32_t width  = m_resolution.m_width;
				uint32_t height = m_resolution.m_height;
				GLenum filter = BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) || BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES < 30)
					? GL_NEAREST
					: GL_LINEAR
					;
				GL_CHECK(glBlitFramebuffer(0
					, 0
					, width
					, height
					, 0
					, 0
					, width
					, height
					, GL_COLOR_BUFFER_BIT
					, filter
					) );
				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_backBufferFbo) );
			}
		}

		void setRenderContextSize(uint32_t _width, uint32_t _height, uint32_t _flags = 0)
		{
			if (_width  != 0
			||  _height != 0)
			{
				if (!m_glctx.isValid() )
				{
					m_glctx.create(_width, _height);

#if BX_PLATFORM_IOS
					// iOS: need to figure out how to deal with FBO created by context.
					m_backBufferFbo = m_msaaBackBufferFbo = m_glctx.getFbo();
#endif // BX_PLATFORM_IOS
				}
				else
				{
					destroyMsaaFbo();

					m_glctx.resize(_width, _height, _flags);

					uint32_t msaa = (_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT;
					msaa = bx::uint32_min(m_maxMsaa, msaa == 0 ? 0 : 1<<msaa);

					createMsaaFbo(_width, _height, msaa);
				}
			}

			m_flip = true;
		}

		void invalidateCache()
		{
			if (m_vaoSupport)
			{
				m_vaoStateCache.invalidate();
			}

			if ( (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) ||  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
			&&  m_samplerObjectSupport)
			{
				m_samplerStateCache.invalidate();
			}
		}

		void setSamplerState(uint32_t _stage, uint32_t _numMips, uint32_t _flags)
		{
			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			||  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
			{
				if (0 == (BGFX_SAMPLER_DEFAULT_FLAGS & _flags) )
				{
					_flags &= ~BGFX_TEXTURE_RESERVED_MASK;
					_flags &= BGFX_TEXTURE_SAMPLER_BITS_MASK;
					_flags |= _numMips<<BGFX_TEXTURE_RESERVED_SHIFT;
					GLuint sampler = m_samplerStateCache.find(_flags);

					if (UINT32_MAX == sampler)
					{
						sampler = m_samplerStateCache.add(_flags);

						GL_CHECK(glSamplerParameteri(sampler
							, GL_TEXTURE_WRAP_S
							, s_textureAddress[(_flags&BGFX_TEXTURE_U_MASK)>>BGFX_TEXTURE_U_SHIFT]
							) );
						GL_CHECK(glSamplerParameteri(sampler
							, GL_TEXTURE_WRAP_T
							, s_textureAddress[(_flags&BGFX_TEXTURE_V_MASK)>>BGFX_TEXTURE_V_SHIFT]
							) );
						GL_CHECK(glSamplerParameteri(sampler
							, GL_TEXTURE_WRAP_R
							, s_textureAddress[(_flags&BGFX_TEXTURE_W_MASK)>>BGFX_TEXTURE_W_SHIFT]
							) );

						GLenum minFilter;
						GLenum magFilter;
						getFilters(_flags, 1 < _numMips, magFilter, minFilter);
						GL_CHECK(glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, magFilter) );
						GL_CHECK(glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter) );

						if (0 != (_flags & (BGFX_TEXTURE_MIN_ANISOTROPIC|BGFX_TEXTURE_MAG_ANISOTROPIC) )
						&&  0.0f < m_maxAnisotropy)
						{
							GL_CHECK(glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, m_maxAnisotropy) );
						}

						if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
						||  m_shadowSamplersSupport)
						{
							const uint32_t cmpFunc = (_flags&BGFX_TEXTURE_COMPARE_MASK)>>BGFX_TEXTURE_COMPARE_SHIFT;
							if (0 == cmpFunc)
							{
								GL_CHECK(glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE) );
							}
							else
							{
								GL_CHECK(glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE) );
								GL_CHECK(glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, s_cmpFunc[cmpFunc]) );
							}
						}
					}

					GL_CHECK(glBindSampler(_stage, sampler) );
				}
				else
				{
					GL_CHECK(glBindSampler(_stage, 0) );
				}
			}
		}

		void ovrPostReset()
		{
#if BGFX_CONFIG_USE_OVR
			if (m_resolution.m_flags & (BGFX_RESET_HMD|BGFX_RESET_HMD_DEBUG) )
			{
				ovrGLConfig config;
				config.OGL.Header.API = ovrRenderAPI_OpenGL;
#	if OVR_VERSION > OVR_VERSION_043
				config.OGL.Header.BackBufferSize.w = m_resolution.m_width;
				config.OGL.Header.BackBufferSize.h = m_resolution.m_height;
#	else
				config.OGL.Header.RTSize.w = m_resolution.m_width;
				config.OGL.Header.RTSize.h = m_resolution.m_height;
#	endif // OVR_VERSION > OVR_VERSION_043
				config.OGL.Header.Multisample = 0;
				config.OGL.Window = (HWND)g_platformData.nwh;
				config.OGL.DC = GetDC(config.OGL.Window);
				if (m_ovr.postReset(g_platformData.nwh, &config.Config, !!(m_resolution.m_flags & BGFX_RESET_HMD_DEBUG) ) )
				{
					uint32_t size = sizeof(uint32_t) + sizeof(TextureCreate);
					const Memory* mem = alloc(size);

					bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
					uint32_t magic = BGFX_CHUNK_MAGIC_TEX;
					bx::write(&writer, magic);

					TextureCreate tc;
					tc.m_flags   = BGFX_TEXTURE_RT|( ((m_resolution.m_flags & BGFX_RESET_MSAA_MASK) >> BGFX_RESET_MSAA_SHIFT) << BGFX_TEXTURE_RT_MSAA_SHIFT);;
					tc.m_width   = m_ovr.m_rtSize.w;
					tc.m_height  = m_ovr.m_rtSize.h;
					tc.m_sides   = 0;
					tc.m_depth   = 0;
					tc.m_numMips = 1;
					tc.m_format  = uint8_t(bgfx::TextureFormat::BGRA8);
					tc.m_cubeMap = false;
					tc.m_mem = NULL;
					bx::write(&writer, tc);

					m_ovrRT.create(mem, tc.m_flags, 0);
					release(mem);

					m_ovrFbo = m_msaaBackBufferFbo;

					GL_CHECK(glGenFramebuffers(1, &m_msaaBackBufferFbo) );
					GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_msaaBackBufferFbo) );

					GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER
						, GL_COLOR_ATTACHMENT0
						, GL_TEXTURE_2D
						, m_ovrRT.m_id
						, 0
						) );

					GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_ovrFbo) );

					ovrGLTexture texture;
					texture.OGL.Header.API         = ovrRenderAPI_OpenGL;
					texture.OGL.Header.TextureSize = m_ovr.m_rtSize;
					texture.OGL.TexId              = m_ovrRT.m_id;
					m_ovr.postReset(texture.Texture);
				}
			}
#endif // BGFX_CONFIG_USE_OVR
		}

		void ovrPreReset()
		{
#if BGFX_CONFIG_USE_OVR
			m_ovr.preReset();
			if (m_ovr.isEnabled() )
			{
				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0) );
				GL_CHECK(glDeleteFramebuffers(1, &m_msaaBackBufferFbo) );
				m_msaaBackBufferFbo = m_ovrFbo;
				m_ovrFbo = 0;
				m_ovrRT.destroy();
			}
#endif // BGFX_CONFIG_USE_OVR
		}

		void updateCapture()
		{
			if (m_resolution.m_flags&BGFX_RESET_CAPTURE)
			{
				m_captureSize = m_resolution.m_width*m_resolution.m_height*4;
				m_capture = BX_REALLOC(g_allocator, m_capture, m_captureSize);
				g_callback->captureBegin(m_resolution.m_width, m_resolution.m_height, m_resolution.m_width*4, TextureFormat::BGRA8, true);
			}
			else
			{
				captureFinish();
			}
		}

		void capture()
		{
			if (NULL != m_capture)
			{
				GL_CHECK(glReadPixels(0
					, 0
					, m_resolution.m_width
					, m_resolution.m_height
					, m_readPixelsFmt
					, GL_UNSIGNED_BYTE
					, m_capture
					) );

				g_callback->captureFrame(m_capture, m_captureSize);
			}
		}

		void captureFinish()
		{
			if (NULL != m_capture)
			{
				g_callback->captureEnd();
				BX_FREE(g_allocator, m_capture);
				m_capture = NULL;
				m_captureSize = 0;
			}
		}

		bool programFetchFromCache(GLuint programId, uint64_t _id)
		{
			_id ^= m_hash;

			bool cached = false;

			if (m_programBinarySupport)
			{
				uint32_t length = g_callback->cacheReadSize(_id);
				cached = length > 0;

				if (cached)
				{
					void* data = BX_ALLOC(g_allocator, length);
					if (g_callback->cacheRead(_id, data, length) )
					{
						bx::MemoryReader reader(data, length);

						GLenum format;
						bx::read(&reader, format);

						GL_CHECK(glProgramBinary(programId, format, reader.getDataPtr(), (GLsizei)reader.remaining() ) );
					}

					BX_FREE(g_allocator, data);
				}

#if BGFX_CONFIG_RENDERER_OPENGL
				GL_CHECK(glProgramParameteri(programId, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE) );
#endif // BGFX_CONFIG_RENDERER_OPENGL
			}

			return cached;
		}

		void programCache(GLuint programId, uint64_t _id)
		{
			_id ^= m_hash;

			if (m_programBinarySupport)
			{
				GLint programLength;
				GLenum format;
				GL_CHECK(glGetProgramiv(programId, GL_PROGRAM_BINARY_LENGTH, &programLength) );

				if (0 < programLength)
				{
					uint32_t length = programLength + 4;
					uint8_t* data = (uint8_t*)BX_ALLOC(g_allocator, length);
					GL_CHECK(glGetProgramBinary(programId, programLength, NULL, &format, &data[4]) );
					*(uint32_t*)data = format;

					g_callback->cacheWrite(_id, data, length);

					BX_FREE(g_allocator, data);
				}
			}
		}

		void commit(ConstantBuffer& _constantBuffer)
		{
			_constantBuffer.reset();

			for (;;)
			{
				uint32_t opcode = _constantBuffer.read();

				if (UniformType::End == opcode)
				{
					break;
				}

				UniformType::Enum type;
				uint16_t ignore;
				uint16_t num;
				uint16_t copy;
				ConstantBuffer::decodeOpcode(opcode, type, ignore, num, copy);

				const char* data;
				if (copy)
				{
					data = _constantBuffer.read(g_uniformTypeSize[type]*num);
				}
				else
				{
					UniformHandle handle;
					memcpy(&handle, _constantBuffer.read(sizeof(UniformHandle) ), sizeof(UniformHandle) );
					data = (const char*)m_uniforms[handle.idx];
				}

				uint32_t loc = _constantBuffer.read();

#define CASE_IMPLEMENT_UNIFORM(_uniform, _glsuffix, _dxsuffix, _type) \
		case UniformType::_uniform: \
				{ \
					_type* value = (_type*)data; \
					GL_CHECK(glUniform##_glsuffix(loc, num, value) ); \
				} \
				break;

#define CASE_IMPLEMENT_UNIFORM_T(_uniform, _glsuffix, _dxsuffix, _type) \
		case UniformType::_uniform: \
				{ \
					_type* value = (_type*)data; \
					GL_CHECK(glUniform##_glsuffix(loc, num, GL_FALSE, value) ); \
				} \
				break;

				switch (type)
				{
//				case ConstantType::Int1:
//					{
//						int* value = (int*)data;
//						BX_TRACE("Int1 sampler %d, loc %d (num %d, copy %d)", *value, loc, num, copy);
//						GL_CHECK(glUniform1iv(loc, num, value) );
//					}
//					break;

				CASE_IMPLEMENT_UNIFORM(Int1, 1iv, I, int);
				CASE_IMPLEMENT_UNIFORM(Vec4, 4fv, F, float);
				CASE_IMPLEMENT_UNIFORM_T(Mat3, Matrix3fv, F, float);
				CASE_IMPLEMENT_UNIFORM_T(Mat4, Matrix4fv, F, float);

				case UniformType::End:
					break;

				default:
					BX_TRACE("%4d: INVALID 0x%08x, t %d, l %d, n %d, c %d", _constantBuffer.getPos(), opcode, type, loc, num, copy);
					break;
				}

#undef CASE_IMPLEMENT_UNIFORM
#undef CASE_IMPLEMENT_UNIFORM_T

			}
		}

		void clearQuad(ClearQuad& _clearQuad, const Rect& _rect, const Clear& _clear, uint32_t _height, const float _palette[][4])
		{
			uint32_t numMrt = 1;
			FrameBufferHandle fbh = m_fbh;
			if (isValid(fbh) )
			{
				const FrameBufferGL& fb = m_frameBuffers[fbh.idx];
				numMrt = bx::uint32_max(1, fb.m_num);
			}

			if (1 == numMrt)
			{
				GLuint flags = 0;
				if (BGFX_CLEAR_COLOR & _clear.m_flags)
				{
					if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
					{
						uint8_t index = (uint8_t)bx::uint32_min(BGFX_CONFIG_MAX_CLEAR_COLOR_PALETTE-1, _clear.m_index[0]);
						const float* rgba = _palette[index];
						const float rr = rgba[0];
						const float gg = rgba[1];
						const float bb = rgba[2];
						const float aa = rgba[3];
						GL_CHECK(glClearColor(rr, gg, bb, aa) );
					}
					else
					{
						float rr = _clear.m_index[0]*1.0f/255.0f;
						float gg = _clear.m_index[1]*1.0f/255.0f;
						float bb = _clear.m_index[2]*1.0f/255.0f;
						float aa = _clear.m_index[3]*1.0f/255.0f;
						GL_CHECK(glClearColor(rr, gg, bb, aa) );
					}

					flags |= GL_COLOR_BUFFER_BIT;
					GL_CHECK(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE) );
				}

				if (BGFX_CLEAR_DEPTH & _clear.m_flags)
				{
					flags |= GL_DEPTH_BUFFER_BIT;
					GL_CHECK(glClearDepth(_clear.m_depth) );
					GL_CHECK(glDepthMask(GL_TRUE) );
				}

				if (BGFX_CLEAR_STENCIL & _clear.m_flags)
				{
					flags |= GL_STENCIL_BUFFER_BIT;
					GL_CHECK(glClearStencil(_clear.m_stencil) );
				}

				if (0 != flags)
				{
					GL_CHECK(glEnable(GL_SCISSOR_TEST) );
					GL_CHECK(glScissor(_rect.m_x, _height-_rect.m_height-_rect.m_y, _rect.m_width, _rect.m_height) );
					GL_CHECK(glClear(flags) );
					GL_CHECK(glDisable(GL_SCISSOR_TEST) );
				}
			}
			else
			{
				const GLuint defaultVao = m_vao;
				if (0 != defaultVao)
				{
					GL_CHECK(glBindVertexArray(defaultVao) );
				}

				GL_CHECK(glDisable(GL_SCISSOR_TEST) );
				GL_CHECK(glDisable(GL_CULL_FACE) );
				GL_CHECK(glDisable(GL_BLEND) );

				GLboolean colorMask = !!(BGFX_CLEAR_COLOR & _clear.m_flags);
				GL_CHECK(glColorMask(colorMask, colorMask, colorMask, colorMask) );

				if (BGFX_CLEAR_DEPTH & _clear.m_flags)
				{
					GL_CHECK(glEnable(GL_DEPTH_TEST) );
					GL_CHECK(glDepthFunc(GL_ALWAYS) );
					GL_CHECK(glDepthMask(GL_TRUE) );
				}
				else
				{
					GL_CHECK(glDisable(GL_DEPTH_TEST) );
				}

				if (BGFX_CLEAR_STENCIL & _clear.m_flags)
				{
					GL_CHECK(glEnable(GL_STENCIL_TEST) );
					GL_CHECK(glStencilFuncSeparate(GL_FRONT_AND_BACK, GL_ALWAYS, _clear.m_stencil,  0xff) );
					GL_CHECK(glStencilOpSeparate(GL_FRONT_AND_BACK, GL_REPLACE, GL_REPLACE, GL_REPLACE) );
				}
				else
				{
					GL_CHECK(glDisable(GL_STENCIL_TEST) );
				}

				VertexBufferGL& vb = m_vertexBuffers[_clearQuad.m_vb->handle.idx];
				VertexDecl& vertexDecl = m_vertexDecls[_clearQuad.m_vb->decl.idx];

				{
					struct Vertex
					{
						float m_x;
						float m_y;
						float m_z;
					};

					Vertex* vertex = (Vertex*)_clearQuad.m_vb->data;
					BX_CHECK(vertexDecl.m_stride == sizeof(Vertex), "Stride/Vertex mismatch (stride %d, sizeof(Vertex) %d)", vertexDecl.m_stride, sizeof(Vertex) );

					const float depth = _clear.m_depth * 2.0f - 1.0f;

					vertex->m_x = -1.0f;
					vertex->m_y = -1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x =  1.0f;
					vertex->m_y = -1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x = -1.0f;
					vertex->m_y =  1.0f;
					vertex->m_z = depth;
					vertex++;
					vertex->m_x =  1.0f;
					vertex->m_y =  1.0f;
					vertex->m_z = depth;
				}

				vb.update(0, 4*_clearQuad.m_decl.m_stride, _clearQuad.m_vb->data);

				GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vb.m_id) );

				ProgramGL& program = m_program[_clearQuad.m_program[numMrt-1].idx];
				GL_CHECK(glUseProgram(program.m_id) );
				program.bindAttributes(vertexDecl, 0);

				if (BGFX_CLEAR_COLOR_USE_PALETTE & _clear.m_flags)
				{
					float mrtClear[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS][4];
					for (uint32_t ii = 0; ii < numMrt; ++ii)
					{
						uint8_t index = (uint8_t)bx::uint32_min(BGFX_CONFIG_MAX_CLEAR_COLOR_PALETTE-1, _clear.m_index[ii]);
						memcpy(mrtClear[ii], _palette[index], 16);
					}

					GL_CHECK(glUniform4fv(0, numMrt, mrtClear[0]) );
				}
				else
				{
					float rgba[4] =
					{
						_clear.m_index[0]*1.0f/255.0f,
						_clear.m_index[1]*1.0f/255.0f,
						_clear.m_index[2]*1.0f/255.0f,
						_clear.m_index[3]*1.0f/255.0f,
					};
					GL_CHECK(glUniform4fv(0, 1, rgba) );
				}

				GL_CHECK(glDrawArrays(GL_TRIANGLE_STRIP
					, 0
					, 4
					) );
			}
		}

		void* m_renderdocdll;

		uint16_t m_numWindows;
		FrameBufferHandle m_windows[BGFX_CONFIG_MAX_FRAME_BUFFERS];

		IndexBufferGL m_indexBuffers[BGFX_CONFIG_MAX_INDEX_BUFFERS];
		VertexBufferGL m_vertexBuffers[BGFX_CONFIG_MAX_VERTEX_BUFFERS];
		ShaderGL m_shaders[BGFX_CONFIG_MAX_SHADERS];
		ProgramGL m_program[BGFX_CONFIG_MAX_PROGRAMS];
		TextureGL m_textures[BGFX_CONFIG_MAX_TEXTURES];
		VertexDecl m_vertexDecls[BGFX_CONFIG_MAX_VERTEX_DECLS];
		FrameBufferGL m_frameBuffers[BGFX_CONFIG_MAX_FRAME_BUFFERS];
		UniformRegistry m_uniformReg;
		void* m_uniforms[BGFX_CONFIG_MAX_UNIFORMS];
		TimerQueryGL m_gpuTimer;

		VaoStateCache m_vaoStateCache;
		SamplerStateCache m_samplerStateCache;

		TextVideoMem m_textVideoMem;
		bool m_rtMsaa;

		FrameBufferHandle m_fbh;
		uint16_t m_fbDiscard;

		Resolution m_resolution;
		void* m_capture;
		uint32_t m_captureSize;
		float m_maxAnisotropy;
		float m_maxAnisotropyDefault;
		int32_t m_maxMsaa;
		GLuint m_vao;
		bool m_vaoSupport;
		bool m_samplerObjectSupport;
		bool m_shadowSamplersSupport;
		bool m_programBinarySupport;
		bool m_textureSwizzleSupport;
		bool m_depthTextureSupport;
		bool m_timerQuerySupport;
		bool m_flip;

		uint64_t m_hash;

		GLenum m_readPixelsFmt;
		GLuint m_backBufferFbo;
		GLuint m_msaaBackBufferFbo;
		GLuint m_msaaBackBufferRbos[2];
		GlContext m_glctx;

		const char* m_vendor;
		const char* m_renderer;
		const char* m_version;
		const char* m_glslVersion;

		OVR m_ovr;
		TextureGL m_ovrRT;
		GLint m_ovrFbo;
	};

	RendererContextGL* s_renderGL;

	RendererContextI* rendererCreate()
	{
		s_renderGL = BX_NEW(g_allocator, RendererContextGL);
		s_renderGL->init();
		return s_renderGL;
	}

	void rendererDestroy()
	{
		s_renderGL->shutdown();
		BX_DELETE(g_allocator, s_renderGL);
		s_renderGL = NULL;
	}

	const char* glslTypeName(GLuint _type)
	{
#define GLSL_TYPE(_ty) case _ty: return #_ty

		switch (_type)
		{
			GLSL_TYPE(GL_INT);
			GLSL_TYPE(GL_INT_VEC2);
			GLSL_TYPE(GL_INT_VEC3);
			GLSL_TYPE(GL_INT_VEC4);
			GLSL_TYPE(GL_UNSIGNED_INT);
			GLSL_TYPE(GL_UNSIGNED_INT_VEC2);
			GLSL_TYPE(GL_UNSIGNED_INT_VEC3);
			GLSL_TYPE(GL_UNSIGNED_INT_VEC4);
			GLSL_TYPE(GL_FLOAT);
			GLSL_TYPE(GL_FLOAT_VEC2);
			GLSL_TYPE(GL_FLOAT_VEC3);
			GLSL_TYPE(GL_FLOAT_VEC4);
			GLSL_TYPE(GL_FLOAT_MAT2);
			GLSL_TYPE(GL_FLOAT_MAT3);
			GLSL_TYPE(GL_FLOAT_MAT4);

			GLSL_TYPE(GL_SAMPLER_2D);
			GLSL_TYPE(GL_INT_SAMPLER_2D);
			GLSL_TYPE(GL_UNSIGNED_INT_SAMPLER_2D);

			GLSL_TYPE(GL_SAMPLER_3D);
			GLSL_TYPE(GL_INT_SAMPLER_3D);
			GLSL_TYPE(GL_UNSIGNED_INT_SAMPLER_3D);

			GLSL_TYPE(GL_SAMPLER_CUBE);
			GLSL_TYPE(GL_INT_SAMPLER_CUBE);
			GLSL_TYPE(GL_UNSIGNED_INT_SAMPLER_CUBE);

			GLSL_TYPE(GL_SAMPLER_2D_SHADOW);

			GLSL_TYPE(GL_IMAGE_1D);
			GLSL_TYPE(GL_INT_IMAGE_1D);
			GLSL_TYPE(GL_UNSIGNED_INT_IMAGE_1D);

			GLSL_TYPE(GL_IMAGE_2D);
			GLSL_TYPE(GL_INT_IMAGE_2D);
			GLSL_TYPE(GL_UNSIGNED_INT_IMAGE_2D);

			GLSL_TYPE(GL_IMAGE_3D);
			GLSL_TYPE(GL_INT_IMAGE_3D);
			GLSL_TYPE(GL_UNSIGNED_INT_IMAGE_3D);

			GLSL_TYPE(GL_IMAGE_CUBE);
			GLSL_TYPE(GL_INT_IMAGE_CUBE);
			GLSL_TYPE(GL_UNSIGNED_INT_IMAGE_CUBE);
		}

#undef GLSL_TYPE

		BX_CHECK(false, "Unknown GLSL type? %x", _type);
		return "UNKNOWN GLSL TYPE!";
	}

	const char* glEnumName(GLenum _enum)
	{
#define GLENUM(_ty) case _ty: return #_ty

		switch (_enum)
		{
			GLENUM(GL_TEXTURE);
			GLENUM(GL_RENDERBUFFER);

			GLENUM(GL_INVALID_ENUM);
			GLENUM(GL_INVALID_FRAMEBUFFER_OPERATION);
			GLENUM(GL_INVALID_VALUE);
			GLENUM(GL_INVALID_OPERATION);
			GLENUM(GL_OUT_OF_MEMORY);

			GLENUM(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
			GLENUM(GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
//			GLENUM(GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER);
//			GLENUM(GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER);
			GLENUM(GL_FRAMEBUFFER_UNSUPPORTED);
		}

#undef GLENUM

		BX_WARN(false, "Unknown enum? %x", _enum);
		return "<GLenum?>";
	}

	UniformType::Enum convertGlType(GLenum _type)
	{
		switch (_type)
		{
		case GL_INT:
		case GL_UNSIGNED_INT:
			return UniformType::Int1;

		case GL_FLOAT:
		case GL_FLOAT_VEC2:
		case GL_FLOAT_VEC3:
		case GL_FLOAT_VEC4:
			return UniformType::Vec4;

		case GL_FLOAT_MAT2:
			break;

		case GL_FLOAT_MAT3:
			return UniformType::Mat3;

		case GL_FLOAT_MAT4:
			return UniformType::Mat4;

		case GL_SAMPLER_2D:
		case GL_INT_SAMPLER_2D:
		case GL_UNSIGNED_INT_SAMPLER_2D:

		case GL_SAMPLER_3D:
		case GL_INT_SAMPLER_3D:
		case GL_UNSIGNED_INT_SAMPLER_3D:

		case GL_SAMPLER_CUBE:
		case GL_INT_SAMPLER_CUBE:
		case GL_UNSIGNED_INT_SAMPLER_CUBE:

		case GL_SAMPLER_2D_SHADOW:

		case GL_IMAGE_1D:
		case GL_INT_IMAGE_1D:
		case GL_UNSIGNED_INT_IMAGE_1D:

		case GL_IMAGE_2D:
		case GL_INT_IMAGE_2D:
		case GL_UNSIGNED_INT_IMAGE_2D:

		case GL_IMAGE_3D:
		case GL_INT_IMAGE_3D:
		case GL_UNSIGNED_INT_IMAGE_3D:

		case GL_IMAGE_CUBE:
		case GL_INT_IMAGE_CUBE:
		case GL_UNSIGNED_INT_IMAGE_CUBE:
			return UniformType::Int1;
		};

		BX_CHECK(false, "Unrecognized GL type 0x%04x.", _type);
		return UniformType::End;
	}

	void ProgramGL::create(const ShaderGL& _vsh, const ShaderGL& _fsh)
	{
		m_id = glCreateProgram();
		BX_TRACE("Program create: GL%d: GL%d, GL%d", m_id, _vsh.m_id, _fsh.m_id);

		const uint64_t id = (uint64_t(_vsh.m_hash)<<32) | _fsh.m_hash;
		const bool cached = s_renderGL->programFetchFromCache(m_id, id);

		if (!cached)
		{
			GLint linked = 0;
			if (0 != _vsh.m_id)
			{
				GL_CHECK(glAttachShader(m_id, _vsh.m_id) );

				if (0 != _fsh.m_id)
				{
					GL_CHECK(glAttachShader(m_id, _fsh.m_id) );
				}

				GL_CHECK(glLinkProgram(m_id) );
				GL_CHECK(glGetProgramiv(m_id, GL_LINK_STATUS, &linked) );

				if (0 == linked)
				{
					char log[1024];
					GL_CHECK(glGetProgramInfoLog(m_id, sizeof(log), NULL, log) );
					BX_TRACE("%d: %s", linked, log);
				}
			}

			if (0 == linked)
			{
				BX_WARN(0 != _vsh.m_id, "Invalid vertex/compute shader.");
				GL_CHECK(glDeleteProgram(m_id) );
				m_used[0] = Attrib::Count;
				m_id = 0;
				return;
			}

			s_renderGL->programCache(m_id, id);
		}

		init();

		if (!cached)
		{
			// Must be after init, otherwise init might fail to lookup shader
			// info (NVIDIA Tegra 3 OpenGL ES 2.0 14.01003).
			GL_CHECK(glDetachShader(m_id, _vsh.m_id) );

			if (0 != _fsh.m_id)
			{
				GL_CHECK(glDetachShader(m_id, _fsh.m_id) );
			}
		}
	}

	void ProgramGL::destroy()
	{
		if (NULL != m_constantBuffer)
		{
			ConstantBuffer::destroy(m_constantBuffer);
			m_constantBuffer = NULL;
		}
		m_numPredefined = 0;

		if (0 != m_id)
		{
			GL_CHECK(glUseProgram(0) );
			GL_CHECK(glDeleteProgram(m_id) );
			m_id = 0;
		}

		m_vcref.invalidate(s_renderGL->m_vaoStateCache);
	}

	void ProgramGL::init()
	{
		GLint activeAttribs  = 0;
		GLint activeUniforms = 0;
		GLint activeBuffers  = 0;

#if BGFX_CONFIG_RENDERER_OPENGL >= 31
		GL_CHECK(glBindFragDataLocation(m_id, 0, "bgfx_FragColor") );
#endif // BGFX_CONFIG_RENDERER_OPENGL >= 31

		bool piqSupported = true
			&& s_extension[Extension::ARB_program_interface_query     ].m_supported
			&& s_extension[Extension::ARB_shader_storage_buffer_object].m_supported
			;

		if (piqSupported)
		{
			GL_CHECK(glGetProgramInterfaceiv(m_id, GL_PROGRAM_INPUT,   GL_ACTIVE_RESOURCES, &activeAttribs ) );
			GL_CHECK(glGetProgramInterfaceiv(m_id, GL_UNIFORM,         GL_ACTIVE_RESOURCES, &activeUniforms) );
			GL_CHECK(glGetProgramInterfaceiv(m_id, GL_BUFFER_VARIABLE, GL_ACTIVE_RESOURCES, &activeBuffers ) );
		}
		else
		{
			GL_CHECK(glGetProgramiv(m_id, GL_ACTIVE_ATTRIBUTES, &activeAttribs ) );
			GL_CHECK(glGetProgramiv(m_id, GL_ACTIVE_UNIFORMS,   &activeUniforms) );
		}

		GLint max0, max1;
		GL_CHECK(glGetProgramiv(m_id, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &max0) );
		GL_CHECK(glGetProgramiv(m_id, GL_ACTIVE_UNIFORM_MAX_LENGTH,   &max1) );
		uint32_t maxLength = bx::uint32_max(max0, max1);
		char* name = (char*)alloca(maxLength + 1);

		BX_TRACE("Program %d", m_id);
		BX_TRACE("Attributes (%d):", activeAttribs);
		for (int32_t ii = 0; ii < activeAttribs; ++ii)
		{
			GLint size;
			GLenum type;

			GL_CHECK(glGetActiveAttrib(m_id, ii, maxLength + 1, NULL, &size, &type, name) );

			BX_TRACE("\t%s %s is at location %d"
				, glslTypeName(type)
				, name
				, glGetAttribLocation(m_id, name)
				);
		}

		m_numPredefined = 0;
 		m_numSamplers = 0;

		BX_TRACE("Uniforms (%d):", activeUniforms);
		for (int32_t ii = 0; ii < activeUniforms; ++ii)
		{
			struct VariableInfo
			{
				GLenum type;
				GLint  loc;
				GLint  num;
			};
			VariableInfo vi;
			GLenum props[] ={ GL_TYPE, GL_LOCATION, GL_ARRAY_SIZE };

			GLenum gltype;
			GLint num;
			GLint loc;

			if (piqSupported)
			{
				GL_CHECK(glGetProgramResourceiv(m_id
					, GL_UNIFORM
					, ii
					, BX_COUNTOF(props)
					, props
					, BX_COUNTOF(props)
					, NULL
					, (GLint*)&vi
					) );

				GL_CHECK(glGetProgramResourceName(m_id
					, GL_UNIFORM
					, ii
					, maxLength + 1
					, NULL
					, name
					) );

				gltype = vi.type;
				loc    = vi.loc;
				num    = vi.num;
			}
			else
			{
				GL_CHECK(glGetActiveUniform(m_id, ii, maxLength + 1, NULL, &num, &gltype, name) );
				loc = glGetUniformLocation(m_id, name);
			}

			num = bx::uint32_max(num, 1);

			int offset = 0;
			char* array = strchr(name, '[');
			if (NULL != array)
			{
				BX_TRACE("--- %s", name);
				*array = '\0';
				array++;
				char* end = strchr(array, ']');
				if (NULL != end)
				{ // Some devices (Amazon Fire) might not return terminating brace.
					*end = '\0';
					offset = atoi(array);
				}
			}

			switch (gltype)
			{
			case GL_SAMPLER_2D:
			case GL_INT_SAMPLER_2D:
			case GL_UNSIGNED_INT_SAMPLER_2D:

			case GL_SAMPLER_3D:
			case GL_INT_SAMPLER_3D:
			case GL_UNSIGNED_INT_SAMPLER_3D:

			case GL_SAMPLER_CUBE:
			case GL_INT_SAMPLER_CUBE:
			case GL_UNSIGNED_INT_SAMPLER_CUBE:

			case GL_SAMPLER_2D_SHADOW:

			case GL_IMAGE_1D:
			case GL_INT_IMAGE_1D:
			case GL_UNSIGNED_INT_IMAGE_1D:

			case GL_IMAGE_2D:
			case GL_INT_IMAGE_2D:
			case GL_UNSIGNED_INT_IMAGE_2D:

			case GL_IMAGE_3D:
			case GL_INT_IMAGE_3D:
			case GL_UNSIGNED_INT_IMAGE_3D:

			case GL_IMAGE_CUBE:
			case GL_INT_IMAGE_CUBE:
			case GL_UNSIGNED_INT_IMAGE_CUBE:
				BX_TRACE("Sampler #%d at location %d.", m_numSamplers, loc);
				m_sampler[m_numSamplers] = loc;
				m_numSamplers++;
				break;

			default:
				break;
			}

			PredefinedUniform::Enum predefined = nameToPredefinedUniformEnum(name);
			if (PredefinedUniform::Count != predefined)
			{
				m_predefined[m_numPredefined].m_loc   = loc;
				m_predefined[m_numPredefined].m_count = uint16_t(num);
				m_predefined[m_numPredefined].m_type  = uint8_t(predefined);
				m_numPredefined++;
			}
			else
			{
				const UniformInfo* info = s_renderGL->m_uniformReg.find(name);
				if (NULL != info)
				{
					if (NULL == m_constantBuffer)
					{
						m_constantBuffer = ConstantBuffer::create(1024);
					}

					UniformType::Enum type = convertGlType(gltype);
					m_constantBuffer->writeUniformHandle(type, 0, info->m_handle, uint16_t(num) );
					m_constantBuffer->write(loc);
					BX_TRACE("store %s %d", name, info->m_handle);
				}
			}

			BX_TRACE("\tuniform %s %s%s is at location %d, size %d, offset %d"
				, glslTypeName(gltype)
				, name
				, PredefinedUniform::Count != predefined ? "*" : ""
				, loc
				, num
				, offset
				);
			BX_UNUSED(offset);
		}

		if (NULL != m_constantBuffer)
		{
			m_constantBuffer->finish();
		}

		if (piqSupported)
		{
			struct VariableInfo
			{
				GLenum type;
			};
			VariableInfo vi;
			GLenum props[] = { GL_TYPE };

			BX_TRACE("Buffers (%d):", activeBuffers);
			for (int32_t ii = 0; ii < activeBuffers; ++ii)
			{
				GL_CHECK(glGetProgramResourceiv(m_id
					, GL_BUFFER_VARIABLE
					, ii
					, BX_COUNTOF(props)
					, props
					, BX_COUNTOF(props)
					, NULL
					, (GLint*)&vi
					) );

				GL_CHECK(glGetProgramResourceName(m_id
					, GL_BUFFER_VARIABLE
					, ii
					, maxLength + 1
					, NULL
					, name
					) );

				BX_TRACE("\t%s %s at %d"
					, glslTypeName(vi.type)
					, name
					, 0 //vi.loc
					);
			}
		}

		memset(m_attributes, 0xff, sizeof(m_attributes) );
		uint32_t used = 0;
		for (uint8_t ii = 0; ii < Attrib::Count; ++ii)
		{
			GLint loc = glGetAttribLocation(m_id, s_attribName[ii]);
			if (-1 != loc)
			{
				BX_TRACE("attr %s: %d", s_attribName[ii], loc);
				m_attributes[ii] = loc;
				m_used[used++] = ii;
			}
		}
		BX_CHECK(used < BX_COUNTOF(m_used), "Out of bounds %d > array size %d."
				, used
				, BX_COUNTOF(m_used)
				);
		m_used[used] = Attrib::Count;

		used = 0;
		for (uint32_t ii = 0; ii < BX_COUNTOF(s_instanceDataName); ++ii)
		{
			GLuint loc = glGetAttribLocation(m_id, s_instanceDataName[ii]);
			if (GLuint(-1) != loc )
			{
				BX_TRACE("instance data %s: %d", s_instanceDataName[ii], loc);
				m_instanceData[used++] = loc;
			}
		}
		BX_CHECK(used < BX_COUNTOF(m_instanceData), "Out of bounds %d > array size %d."
				, used
				, BX_COUNTOF(m_instanceData)
				);
		m_instanceData[used] = 0xffff;
	}

	void ProgramGL::bindAttributes(const VertexDecl& _vertexDecl, uint32_t _baseVertex) const
	{
		for (uint32_t ii = 0; Attrib::Count != m_used[ii]; ++ii)
		{
			Attrib::Enum attr = Attrib::Enum(m_used[ii]);
			GLint loc = m_attributes[attr];

			uint8_t num;
			AttribType::Enum type;
			bool normalized;
			bool asInt;
			_vertexDecl.decode(attr, num, type, normalized, asInt);

			if (-1 != loc)
			{
				if (UINT16_MAX != _vertexDecl.m_attributes[attr])
				{
					GL_CHECK(glEnableVertexAttribArray(loc) );
					GL_CHECK(glVertexAttribDivisor(loc, 0) );

					uint32_t baseVertex = _baseVertex*_vertexDecl.m_stride + _vertexDecl.m_offset[attr];
					GL_CHECK(glVertexAttribPointer(loc, num, s_attribType[type], normalized, _vertexDecl.m_stride, (void*)(uintptr_t)baseVertex) );
				}
				else
				{
					GL_CHECK(glDisableVertexAttribArray(loc) );
				}
			}
		}
	}

	void ProgramGL::bindInstanceData(uint32_t _stride, uint32_t _baseVertex) const
	{
		uint32_t baseVertex = _baseVertex;
		for (uint32_t ii = 0; 0xffff != m_instanceData[ii]; ++ii)
		{
			GLint loc = m_instanceData[ii];
			GL_CHECK(glEnableVertexAttribArray(loc) );
			GL_CHECK(glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, _stride, (void*)(uintptr_t)baseVertex) );
			GL_CHECK(glVertexAttribDivisor(loc, 1) );
			baseVertex += 16;
		}
	}

	void IndexBufferGL::destroy()
	{
		GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0) );
		GL_CHECK(glDeleteBuffers(1, &m_id) );

		m_vcref.invalidate(s_renderGL->m_vaoStateCache);
	}

	void VertexBufferGL::destroy()
	{
		GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0) );
		GL_CHECK(glDeleteBuffers(1, &m_id) );

		m_vcref.invalidate(s_renderGL->m_vaoStateCache);
	}

	static void texImage(GLenum _target, GLint _level, GLint _internalFormat, GLsizei _width, GLsizei _height, GLsizei _depth, GLint _border, GLenum _format, GLenum _type, const GLvoid* _data)
	{
		if (_target == GL_TEXTURE_3D)
		{
			GL_CHECK(glTexImage3D(_target, _level, _internalFormat, _width, _height, _depth, _border, _format, _type, _data) );
		}
		else
		{
			BX_UNUSED(_depth);
			GL_CHECK(glTexImage2D(_target, _level, _internalFormat, _width, _height, _border, _format, _type, _data) );
		}
	}

	static void texSubImage(GLenum _target, GLint _level, GLint _xoffset, GLint _yoffset, GLint _zoffset, GLsizei _width, GLsizei _height, GLsizei _depth, GLenum _format, GLenum _type, const GLvoid* _data)
	{
		if (_target == GL_TEXTURE_3D)
		{
			GL_CHECK(glTexSubImage3D(_target, _level, _xoffset, _yoffset, _zoffset, _width, _height, _depth, _format, _type, _data) );
		}
		else
		{
			BX_UNUSED(_zoffset, _depth);
			GL_CHECK(glTexSubImage2D(_target, _level, _xoffset, _yoffset, _width, _height, _format, _type, _data) );
		}
	}

	static void compressedTexImage(GLenum _target, GLint _level, GLenum _internalformat, GLsizei _width, GLsizei _height, GLsizei _depth, GLint _border, GLsizei _imageSize, const GLvoid* _data)
	{
		if (_target == GL_TEXTURE_3D)
		{
			GL_CHECK(glCompressedTexImage3D(_target, _level, _internalformat, _width, _height, _depth, _border, _imageSize, _data) );
		}
		else
		{
			BX_UNUSED(_depth);
			GL_CHECK(glCompressedTexImage2D(_target, _level, _internalformat, _width, _height, _border, _imageSize, _data) );
		}
	}

	static void compressedTexSubImage(GLenum _target, GLint _level, GLint _xoffset, GLint _yoffset, GLint _zoffset, GLsizei _width, GLsizei _height, GLsizei _depth, GLenum _format, GLsizei _imageSize, const GLvoid* _data)
	{
		if (_target == GL_TEXTURE_3D)
		{
			GL_CHECK(glCompressedTexSubImage3D(_target, _level, _xoffset, _yoffset, _zoffset, _width, _height, _depth, _format, _imageSize, _data) );
		}
		else
		{
			BX_UNUSED(_zoffset, _depth);
			GL_CHECK(glCompressedTexSubImage2D(_target, _level, _xoffset, _yoffset, _width, _height, _format, _imageSize, _data) );
		}
	}

	bool TextureGL::init(GLenum _target, uint32_t _width, uint32_t _height, uint32_t _depth, uint8_t _format, uint8_t _numMips, uint32_t _flags)
	{
		m_target  = _target;
		m_numMips = _numMips;
		m_flags   = _flags;
		m_width   = _width;
		m_height  = _height;
		m_depth   = _depth;
		m_currentFlags    = UINT32_MAX;
		m_requestedFormat = _format;
		m_textureFormat   = _format;

		const bool bufferOnly   = 0 != (m_flags&BGFX_TEXTURE_RT_BUFFER_ONLY);
		const bool computeWrite = 0 != (m_flags&BGFX_TEXTURE_COMPUTE_WRITE );

		if (!bufferOnly)
		{
			GL_CHECK(glGenTextures(1, &m_id) );
			BX_CHECK(0 != m_id, "Failed to generate texture id.");
			GL_CHECK(glBindTexture(_target, m_id) );

			const TextureFormatInfo& tfi = s_textureFormat[_format];
			m_fmt  = tfi.m_fmt;
			m_type = tfi.m_type;

			const bool swizzle = true
				&& TextureFormat::BGRA8 == m_requestedFormat
				&& !s_textureFormat[m_requestedFormat].m_supported
				&& !s_renderGL->m_textureSwizzleSupport
				;
			const bool compressed = isCompressed(TextureFormat::Enum(m_requestedFormat) );
			const bool convert    = false
				|| (compressed && m_textureFormat != m_requestedFormat)
				|| swizzle
				|| !s_textureFormat[m_requestedFormat].m_supported
				;

			if (convert)
			{
				m_textureFormat = (uint8_t)TextureFormat::RGBA8;
				const TextureFormatInfo& tfiRgba8 = s_textureFormat[TextureFormat::RGBA8];
				m_fmt  = tfiRgba8.m_fmt;
				m_type = tfiRgba8.m_type;
			}

			if (computeWrite)
			{
				if (_target == GL_TEXTURE_3D)
				{
					GL_CHECK(glTexStorage3D(_target
							, _numMips
							, s_textureFormat[m_textureFormat].m_internalFmt
							, m_width
							, m_height
							, _depth
							) );
				}
				else
				{
					GL_CHECK(glTexStorage2D(_target
							, _numMips
							, s_textureFormat[m_textureFormat].m_internalFmt
							, m_width
							, m_height
							) );
				}
			}

			setSamplerState(_flags);

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			&&  TextureFormat::BGRA8 == m_requestedFormat
			&&  !s_textureFormat[m_requestedFormat].m_supported
			&&  s_renderGL->m_textureSwizzleSupport)
			{
				GLint swizzleMask[] = { GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA };
				GL_CHECK(glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask) );
			}
		}

		const bool renderTarget = 0 != (m_flags&BGFX_TEXTURE_RT_MASK);

		if (renderTarget)
		{
			uint32_t msaaQuality = ( (m_flags&BGFX_TEXTURE_RT_MSAA_MASK)>>BGFX_TEXTURE_RT_MSAA_SHIFT);
			msaaQuality = bx::uint32_satsub(msaaQuality, 1);
			msaaQuality = bx::uint32_min(s_renderGL->m_maxMsaa, msaaQuality == 0 ? 0 : 1<<msaaQuality);

			if (0 != msaaQuality
			||  bufferOnly)
			{
				GL_CHECK(glGenRenderbuffers(1, &m_rbo) );
				BX_CHECK(0 != m_rbo, "Failed to generate renderbuffer id.");
				GL_CHECK(glBindRenderbuffer(GL_RENDERBUFFER, m_rbo) );

				if (0 == msaaQuality)
				{
					GL_CHECK(glRenderbufferStorage(GL_RENDERBUFFER
						, s_rboFormat[m_textureFormat]
						, _width
						, _height
						) );
				}
				else if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
				{
					GL_CHECK(glRenderbufferStorageMultisample(GL_RENDERBUFFER
						, msaaQuality
						, s_rboFormat[m_textureFormat]
						, _width
						, _height
						) );
				}

				GL_CHECK(glBindRenderbuffer(GL_RENDERBUFFER, 0) );

				if (bufferOnly)
				{
					// This is render buffer, there is no sampling, no need
					// to create texture.
					return false;
				}
			}
		}

		return true;
	}

	void TextureGL::create(const Memory* _mem, uint32_t _flags, uint8_t _skip)
	{
		ImageContainer imageContainer;

		if (imageParse(imageContainer, _mem->data, _mem->size) )
		{
			uint8_t numMips = imageContainer.m_numMips;
			const uint8_t startLod = uint8_t(bx::uint32_min(_skip, numMips-1) );
			numMips -= startLod;
			uint32_t textureWidth;
			uint32_t textureHeight;
			uint32_t textureDepth;
			{
				const ImageBlockInfo& ibi = getBlockInfo(TextureFormat::Enum(imageContainer.m_format) );
				textureWidth  = bx::uint32_max(ibi.blockWidth,  imageContainer.m_width >>startLod);
				textureHeight = bx::uint32_max(ibi.blockHeight, imageContainer.m_height>>startLod);
				textureDepth  = imageContainer.m_depth;
			}

			GLenum target = GL_TEXTURE_2D;
			if (imageContainer.m_cubeMap)
			{
				target = GL_TEXTURE_CUBE_MAP;
			}
			else if (imageContainer.m_depth > 1)
			{
				target = GL_TEXTURE_3D;
			}

			if (!init(target
					, textureWidth
					, textureHeight
					, textureDepth
					, imageContainer.m_format
					, numMips
					, _flags
					) )
			{
				return;
			}

			const bool computeWrite = 0 != (m_flags&BGFX_TEXTURE_COMPUTE_WRITE);
			const bool srgb         = 0 != (m_flags&BGFX_TEXTURE_SRGB);

			target = GL_TEXTURE_CUBE_MAP == m_target ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : m_target;

			const GLenum internalFmt = srgb
				? s_textureFormat[m_textureFormat].m_internalFmtSrgb
				: s_textureFormat[m_textureFormat].m_internalFmt
				;

			const bool swizzle = true
				&& TextureFormat::BGRA8 == m_requestedFormat
				&& !s_textureFormat[m_requestedFormat].m_supported
				&& !s_renderGL->m_textureSwizzleSupport
				;
			const bool compressed = isCompressed(TextureFormat::Enum(m_requestedFormat) );
			const bool convert    = false
				|| m_textureFormat != m_requestedFormat
				|| swizzle
				;

			BX_TRACE("Texture%-4s %3d: %s (requested: %s), %dx%dx%d%s."
				, imageContainer.m_cubeMap ? "Cube" : (1 < imageContainer.m_depth ? "3D" : "2D")
				, this - s_renderGL->m_textures
				, getName( (TextureFormat::Enum)m_textureFormat)
				, getName( (TextureFormat::Enum)m_requestedFormat)
				, textureWidth
				, textureHeight
				, imageContainer.m_cubeMap ? 6 : (1 < imageContainer.m_depth ? imageContainer.m_depth : 0)
				, 0 != (m_flags&BGFX_TEXTURE_RT_MASK) ? " (render target)" : ""
				);

			BX_WARN(!convert, "Texture %s%s%s from %s to %s."
					, swizzle ? "swizzle" : ""
					, swizzle&&convert ? " and " : ""
					, convert ? "convert" : ""
					, getName( (TextureFormat::Enum)m_requestedFormat)
					, getName( (TextureFormat::Enum)m_textureFormat)
					);

			uint8_t* temp = NULL;
			if (convert)
			{
				temp = (uint8_t*)BX_ALLOC(g_allocator, textureWidth*textureHeight*4);
			}

			for (uint8_t side = 0, numSides = imageContainer.m_cubeMap ? 6 : 1; side < numSides; ++side)
			{
				uint32_t width  = textureWidth;
				uint32_t height = textureHeight;
				uint32_t depth  = imageContainer.m_depth;

				for (uint8_t lod = 0, num = numMips; lod < num; ++lod)
				{
					width  = bx::uint32_max(1, width);
					height = bx::uint32_max(1, height);
					depth  = bx::uint32_max(1, depth);

					ImageMip mip;
					if (imageGetRawData(imageContainer, side, lod+startLod, _mem->data, _mem->size, mip) )
					{
						if (compressed
						&& !convert)
						{
							compressedTexImage(target+side
								, lod
								, internalFmt
								, width
								, height
								, depth
								, 0
								, mip.m_size
								, mip.m_data
								);
						}
						else
						{
							const uint8_t* data = mip.m_data;

							if (convert)
							{
								imageDecodeToRgba8(temp
										, mip.m_data
										, mip.m_width
										, mip.m_height
										, mip.m_width*4
										, mip.m_format
										);
								data = temp;
							}

							texImage(target+side
								, lod
								, internalFmt
								, width
								, height
								, depth
								, 0
								, m_fmt
								, m_type
								, data
								);
						}
					}
					else if (!computeWrite)
					{
						if (compressed)
						{
							uint32_t size = bx::uint32_max(1, (width  + 3)>>2)
										  * bx::uint32_max(1, (height + 3)>>2)
										  * 4*4*getBitsPerPixel(TextureFormat::Enum(m_textureFormat) )/8
										  ;

							compressedTexImage(target+side
								, lod
								, internalFmt
								, width
								, height
								, depth
								, 0
								, size
								, NULL
								);
						}
						else
						{
							texImage(target+side
								, lod
								, internalFmt
								, width
								, height
								, depth
								, 0
								, m_fmt
								, m_type
								, NULL
								);
						}
					}

					width  >>= 1;
					height >>= 1;
					depth  >>= 1;
				}
			}

			if (NULL != temp)
			{
				BX_FREE(g_allocator, temp);
			}
		}

		GL_CHECK(glBindTexture(m_target, 0) );
	}

	void TextureGL::destroy()
	{
		if (0 != m_id)
		{
			GL_CHECK(glBindTexture(m_target, 0) );
			GL_CHECK(glDeleteTextures(1, &m_id) );
			m_id = 0;
		}

		if (0 != m_rbo)
		{
			GL_CHECK(glDeleteRenderbuffers(1, &m_rbo) );
			m_rbo = 0;
		}
	}

	void TextureGL::update(uint8_t _side, uint8_t _mip, const Rect& _rect, uint16_t _z, uint16_t _depth, uint16_t _pitch, const Memory* _mem)
	{
		BX_UNUSED(_z, _depth);

		const uint32_t bpp = getBitsPerPixel(TextureFormat::Enum(m_textureFormat) );
		const uint32_t rectpitch = _rect.m_width*bpp/8;
		uint32_t srcpitch  = UINT16_MAX == _pitch ? rectpitch : _pitch;

		GL_CHECK(glBindTexture(m_target, m_id) );
		GL_CHECK(glPixelStorei(GL_UNPACK_ALIGNMENT, 1) );

		GLenum target = GL_TEXTURE_CUBE_MAP == m_target ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : m_target;

		const bool swizzle = true
			&& TextureFormat::BGRA8 == m_requestedFormat
			&& !s_textureFormat[m_requestedFormat].m_supported
			&& !s_renderGL->m_textureSwizzleSupport
			;
		const bool unpackRowLength = BX_IGNORE_C4127(!!BGFX_CONFIG_RENDERER_OPENGL || s_extension[Extension::EXT_unpack_subimage].m_supported);
		const bool compressed      = isCompressed(TextureFormat::Enum(m_requestedFormat) );
		const bool convert         = false
			|| (compressed && m_textureFormat != m_requestedFormat)
			|| swizzle
			;

		const uint32_t width  = _rect.m_width;
		const uint32_t height = _rect.m_height;

		uint8_t* temp = NULL;
		if (convert
		||  !unpackRowLength)
		{
			temp = (uint8_t*)BX_ALLOC(g_allocator, rectpitch*height);
		}
		else if (unpackRowLength)
		{
			GL_CHECK(glPixelStorei(GL_UNPACK_ROW_LENGTH, srcpitch*8/bpp) );
		}

		if (compressed)
		{
			const uint8_t* data = _mem->data;

			if (!unpackRowLength)
			{
				imageCopy(width, height, bpp, srcpitch, data, temp);
				data = temp;
			}

			GL_CHECK(compressedTexSubImage(target+_side
				, _mip
				, _rect.m_x
				, _rect.m_y
				, _z
				, _rect.m_width
				, _rect.m_height
				, _depth
				, m_fmt
				, _mem->size
				, data
				) );
		}
		else
		{
			const uint8_t* data = _mem->data;

			if (convert)
			{
				imageDecodeToRgba8(temp, data, width, height, srcpitch, m_requestedFormat);
				data = temp;
				srcpitch = rectpitch;
			}

			if (!unpackRowLength
			&&  !convert)
			{
				imageCopy(width, height, bpp, srcpitch, data, temp);
				data = temp;
			}

			GL_CHECK(texSubImage(target+_side
				, _mip
				, _rect.m_x
				, _rect.m_y
				, _z
				, _rect.m_width
				, _rect.m_height
				, _depth
				, m_fmt
				, m_type
				, data
				) );
		}

		if (!convert
		&&  unpackRowLength)
		{
			GL_CHECK(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0) );
		}

		if (NULL != temp)
		{
			BX_FREE(g_allocator, temp);
		}
	}

	void TextureGL::setSamplerState(uint32_t _flags)
	{
		if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES < 30)
		&&  !s_textureFilter[m_textureFormat])
		{
			// Force point sampling when texture format doesn't support linear sampling.
			_flags &= 0
				| BGFX_TEXTURE_MIN_MASK
				| BGFX_TEXTURE_MAG_MASK
				| BGFX_TEXTURE_MIP_MASK
				;
			_flags |= 0
				| BGFX_TEXTURE_MIN_POINT
				| BGFX_TEXTURE_MAG_POINT
				| BGFX_TEXTURE_MIP_POINT
				;
		}

		const uint32_t flags = (0 != (BGFX_SAMPLER_DEFAULT_FLAGS & _flags) ? m_flags : _flags) & BGFX_TEXTURE_SAMPLER_BITS_MASK;
		if (flags != m_currentFlags)
		{
			const GLenum target = m_target;
			const uint8_t numMips = m_numMips;

			GL_CHECK(glTexParameteri(target, GL_TEXTURE_WRAP_S, s_textureAddress[(flags&BGFX_TEXTURE_U_MASK)>>BGFX_TEXTURE_U_SHIFT]) );
			GL_CHECK(glTexParameteri(target, GL_TEXTURE_WRAP_T, s_textureAddress[(flags&BGFX_TEXTURE_V_MASK)>>BGFX_TEXTURE_V_SHIFT]) );

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 30)
			||  s_extension[Extension::APPLE_texture_max_level].m_supported)
			{
				GL_CHECK(glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, numMips-1) );
			}

			if (target == GL_TEXTURE_3D)
			{
				GL_CHECK(glTexParameteri(target, GL_TEXTURE_WRAP_R, s_textureAddress[(flags&BGFX_TEXTURE_W_MASK)>>BGFX_TEXTURE_W_SHIFT]) );
			}

			GLenum magFilter;
			GLenum minFilter;
			getFilters(flags, 1 < numMips, magFilter, minFilter);
			GL_CHECK(glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter) );
			GL_CHECK(glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter) );
			if (0 != (flags & (BGFX_TEXTURE_MIN_ANISOTROPIC|BGFX_TEXTURE_MAG_ANISOTROPIC) )
			&&  0.0f < s_renderGL->m_maxAnisotropy)
			{
				GL_CHECK(glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, s_renderGL->m_maxAnisotropy) );
			}

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30)
			||  s_renderGL->m_shadowSamplersSupport)
			{
				const uint32_t cmpFunc = (flags&BGFX_TEXTURE_COMPARE_MASK)>>BGFX_TEXTURE_COMPARE_SHIFT;
				if (0 == cmpFunc)
				{
					GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_COMPARE_MODE, GL_NONE) );
				}
				else
				{
					GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE) );
					GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_COMPARE_FUNC, s_cmpFunc[cmpFunc]) );
				}
			}

			m_currentFlags = flags;
		}
	}

	void TextureGL::commit(uint32_t _stage, uint32_t _flags)
	{
		GL_CHECK(glActiveTexture(GL_TEXTURE0+_stage) );
		GL_CHECK(glBindTexture(m_target, m_id) );

		if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES)
		&&  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES < 30) )
		{
			// GLES2 doesn't have support for sampler object.
			setSamplerState(_flags);
		}
		else if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
			 &&  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL < 31) )
		{
			// In case that GL 2.1 sampler object is supported via extension.
			if (s_renderGL->m_samplerObjectSupport)
			{
				s_renderGL->setSamplerState(_stage, m_numMips, _flags);
			}
			else
			{
				setSamplerState(_flags);
			}
		}
		else
		{
			// Everything else has sampler object.
			s_renderGL->setSamplerState(_stage, m_numMips, _flags);
		}
	}

	void writeString(bx::WriterI* _writer, const char* _str)
	{
		bx::write(_writer, _str, (int32_t)strlen(_str) );
	}

	void writeStringf(bx::WriterI* _writer, const char* _format, ...)
	{
		char temp[512];

		va_list argList;
		va_start(argList, _format);
		int len = bx::vsnprintf(temp, BX_COUNTOF(temp), _format, argList);
		va_end(argList);

		bx::write(_writer, temp, len);
	}

	void strins(char* _str, const char* _insert)
	{
		size_t len = strlen(_insert);
		memmove(&_str[len], _str, strlen(_str)+1);
		memcpy(_str, _insert, len);
	}

	void ShaderGL::create(Memory* _mem)
	{
		bx::MemoryReader reader(_mem->data, _mem->size);
		m_hash = bx::hashMurmur2A(_mem->data, _mem->size);

		uint32_t magic;
		bx::read(&reader, magic);

		switch (magic)
		{
		case BGFX_CHUNK_MAGIC_CSH: m_type = GL_COMPUTE_SHADER;  break;
		case BGFX_CHUNK_MAGIC_FSH: m_type = GL_FRAGMENT_SHADER;	break;
		case BGFX_CHUNK_MAGIC_VSH: m_type = GL_VERTEX_SHADER;   break;

		default:
			BGFX_FATAL(false, Fatal::InvalidShader, "Unknown shader format %x.", magic);
			break;
		}

		uint32_t iohash;
		bx::read(&reader, iohash);

		uint16_t count;
		bx::read(&reader, count);

		BX_TRACE("%s Shader consts %d"
			, BGFX_CHUNK_MAGIC_FSH == magic ? "Fragment" : BGFX_CHUNK_MAGIC_VSH == magic ? "Vertex" : "Compute"
			, count
			);

		for (uint32_t ii = 0; ii < count; ++ii)
		{
			uint8_t nameSize;
			bx::read(&reader, nameSize);

			char name[256];
			bx::read(&reader, &name, nameSize);
			name[nameSize] = '\0';

			uint8_t type;
			bx::read(&reader, type);

			uint8_t num;
			bx::read(&reader, num);

			uint16_t regIndex;
			bx::read(&reader, regIndex);

			uint16_t regCount;
			bx::read(&reader, regCount);
		}

		uint32_t shaderSize;
		bx::read(&reader, shaderSize);

		m_id = glCreateShader(m_type);
		BX_WARN(0 != m_id, "Failed to create %s shader."
				, BGFX_CHUNK_MAGIC_FSH == magic ? "fragment" : BGFX_CHUNK_MAGIC_VSH == magic ? "vertex" : "compute"
				);

		const char* code = (const char*)reader.getDataPtr();

		if (0 != m_id)
		{
			if (GL_COMPUTE_SHADER != m_type)
			{
				int32_t codeLen = (int32_t)strlen(code);
				int32_t tempLen = codeLen + (4<<10);
				char* temp = (char*)alloca(tempLen);
				bx::StaticMemoryBlockWriter writer(temp, tempLen);

				if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES)
				&&  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES < 30) )
				{
					writeString(&writer
						, "#define flat\n"
						  "#define smooth\n"
						  "#define noperspective\n"
						);

					bool usesDerivatives = s_extension[Extension::OES_standard_derivatives].m_supported
						&& bx::findIdentifierMatch(code, s_OES_standard_derivatives)
						;

					bool usesFragData  = !!bx::findIdentifierMatch(code, "gl_FragData");

					bool usesFragDepth = !!bx::findIdentifierMatch(code, "gl_FragDepth");

					bool usesShadowSamplers = !!bx::findIdentifierMatch(code, s_EXT_shadow_samplers);

					bool usesTexture3D = s_extension[Extension::OES_texture_3D].m_supported
						&& bx::findIdentifierMatch(code, s_OES_texture_3D)
						;

					bool usesTextureLod = !!bx::findIdentifierMatch(code, s_EXT_shader_texture_lod);

					bool usesFragmentOrdering = !!bx::findIdentifierMatch(code, "beginFragmentShaderOrdering");

					if (usesDerivatives)
					{
						writeString(&writer, "#extension GL_OES_standard_derivatives : enable\n");
					}

					if (usesFragData)
					{
						BX_WARN(s_extension[Extension::EXT_draw_buffers  ].m_supported
							||  s_extension[Extension::WEBGL_draw_buffers].m_supported
							, "EXT_draw_buffers is used but not supported by GLES2 driver."
							);
						writeString(&writer
							, "#extension GL_EXT_draw_buffers : enable\n"
							);
					}

					bool insertFragDepth = false;
					if (usesFragDepth)
					{
						BX_WARN(s_extension[Extension::EXT_frag_depth].m_supported, "EXT_frag_depth is used but not supported by GLES2 driver.");
						if (s_extension[Extension::EXT_frag_depth].m_supported)
						{
							writeString(&writer
								, "#extension GL_EXT_frag_depth : enable\n"
								  "#define bgfx_FragDepth gl_FragDepthEXT\n"
								);

							char str[128];
							bx::snprintf(str, BX_COUNTOF(str), "%s float gl_FragDepthEXT;\n"
								, s_extension[Extension::OES_fragment_precision_high].m_supported ? "highp" : "mediump"
								);
							writeString(&writer, str);
						}
						else
						{
							insertFragDepth = true;
						}
					}

					if (usesShadowSamplers)
					{
						if (s_renderGL->m_shadowSamplersSupport)
						{
							writeString(&writer
								, "#extension GL_EXT_shadow_samplers : enable\n"
								  "#define shadow2D shadow2DEXT\n"
								  "#define shadow2DProj shadow2DProjEXT\n"
								);
						}
						else
						{
							writeString(&writer
								, "#define sampler2DShadow sampler2D\n"
								  "#define shadow2D(_sampler, _coord) step(_coord.z, texture2D(_sampler, _coord.xy).x)\n"
								  "#define shadow2DProj(_sampler, _coord) step(_coord.z/_coord.w, texture2DProj(_sampler, _coord).x)\n"
								);
						}
					}

					if (usesTexture3D)
					{
						writeString(&writer, "#extension GL_OES_texture_3D : enable\n");
					}

					if (usesTextureLod)
					{
						BX_WARN(s_extension[Extension::EXT_shader_texture_lod].m_supported, "EXT_shader_texture_lod is used but not supported by GLES2 driver.");
						if (s_extension[Extension::EXT_shader_texture_lod].m_supported
						/*&&  GL_VERTEX_SHADER == m_type*/)
						{
							writeString(&writer
								, "#extension GL_EXT_shader_texture_lod : enable\n"
								  "#define texture2DLod texture2DLodEXT\n"
								  "#define texture2DProjLod texture2DProjLodEXT\n"
								  "#define textureCubeLod textureCubeLodEXT\n"
								);
						}
						else
						{
							writeString(&writer
								, "#define texture2DLod(_sampler, _coord, _level) texture2D(_sampler, _coord)\n"
								  "#define texture2DProjLod(_sampler, _coord, _level) texture2DProj(_sampler, _coord)\n"
								  "#define textureCubeLod(_sampler, _coord, _level) textureCube(_sampler, _coord)\n"
								);
						}
					}

					if (usesFragmentOrdering)
					{
						if (s_extension[Extension::INTEL_fragment_shader_ordering].m_supported)
						{
							writeString(&writer, "#extension GL_INTEL_fragment_shader_ordering : enable\n");
						}
						else
						{
							writeString(&writer, "#define beginFragmentShaderOrdering()\n");
						}
					}

					writeStringf(&writer, "precision %s float;\n"
							, m_type == GL_FRAGMENT_SHADER ? "mediump" : "highp"
							);

					bx::write(&writer, code, codeLen);
					bx::write(&writer, '\0');

					if (insertFragDepth)
					{
						char* entry = strstr(temp, "void main ()");
						if (NULL != entry)
						{
							char* brace = strstr(entry, "{");
							if (NULL != brace)
							{
								const char* end = bx::strmb(brace, '{', '}');
								if (NULL != end)
								{
									strins(brace+1, "\n  float bgfx_FragDepth = 0.0;\n");
								}
							}
						}
					}

					// Replace all instances of gl_FragDepth with bgfx_FragDepth.
					for (const char* fragDepth = bx::findIdentifierMatch(temp, "gl_FragDepth"); NULL != fragDepth; fragDepth = bx::findIdentifierMatch(fragDepth, "gl_FragDepth") )
					{
						char* insert = const_cast<char*>(fragDepth);
						strins(insert, "bg");
						memcpy(insert + 2, "fx", 2);
					}
				}
				else if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
					 &&  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL <= 21) )
				{
					bool usesTextureLod = true
						&& s_extension[Extension::ARB_shader_texture_lod].m_supported
						&& bx::findIdentifierMatch(code, s_ARB_shader_texture_lod)
						;

					bool usesIUsamplers = !!bx::findIdentifierMatch(code, s_uisamplers);

					uint32_t version = usesIUsamplers
						? 130
						: (usesTextureLod ? 120 : 0)
						;

					if (0 != version)
					{
						writeStringf(&writer, "#version %d\n", version);
					}

					if (usesTextureLod)
					{
						if (m_type == GL_FRAGMENT_SHADER)
						{
							writeString(&writer, "#extension GL_ARB_shader_texture_lod : enable\n");
						}
					}

					if (130 <= version)
					{
						if (m_type == GL_FRAGMENT_SHADER)
						{
							writeString(&writer, "#define varying in\n");
						}
						else
						{
							writeString(&writer, "#define attribute in\n");
							writeString(&writer, "#define varying out\n");
						}

						uint32_t fragData = 0;

						if (!!bx::findIdentifierMatch(code, "gl_FragData") )
						{
							for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
							{
								char tmpFragData[16];
								bx::snprintf(tmpFragData, BX_COUNTOF(tmpFragData), "gl_FragData[%d]", ii);
								fragData = bx::uint32_max(fragData, NULL == strstr(code, tmpFragData) ? 0 : ii+1);
							}

							BGFX_FATAL(0 != fragData, Fatal::InvalidShader, "Unable to find and patch gl_FragData!");
						}

						if (0 != fragData)
						{
							writeStringf(&writer, "out vec4 bgfx_FragData[%d];\n", fragData);
							writeString(&writer, "#define gl_FragData bgfx_FragData\n");
						}
						else
						{
							writeString(&writer, "out vec4 bgfx_FragColor;\n");
							writeString(&writer, "#define gl_FragColor bgfx_FragColor\n");
						}
					}

					writeString(&writer
							, "#define lowp\n"
							  "#define mediump\n"
							  "#define highp\n"
							  "#define flat\n"
							  "#define smooth\n"
							  "#define noperspective\n"
							);

					bx::write(&writer, code, codeLen);
					bx::write(&writer, '\0');
				}
				else if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL   >= 31)
					 ||  BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
				{
					if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
					{
						writeString(&writer
							, "#version 300 es\n"
							  "precision mediump float;\n"
							);
					}
					else
					{
						writeString(&writer, "#version 140\n");
					}

					writeString(&writer, "#define texture2DLod textureLod\n");
					writeString(&writer, "#define texture3DLod textureLod\n");
					writeString(&writer, "#define textureCubeLod textureLod\n");

					if (m_type == GL_FRAGMENT_SHADER)
					{
						writeString(&writer, "#define varying in\n");
						writeString(&writer, "#define texture2D texture\n");
						writeString(&writer, "#define texture2DProj textureProj\n");

						if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) )
						{
							writeString(&writer, "#define shadow2D(_sampler, _coord) vec2(textureProj(_sampler, vec4(_coord, 1.0) ) )\n");
							writeString(&writer, "#define shadow2DProj(_sampler, _coord) vec2(textureProj(_sampler, _coord) ) )\n");
						}
						else
						{
							writeString(&writer, "#define shadow2D(_sampler, _coord) (textureProj(_sampler, vec4(_coord, 1.0) ) )\n");
							writeString(&writer, "#define shadow2DProj(_sampler, _coord) (textureProj(_sampler, _coord) ) )\n");
						}

						writeString(&writer, "#define texture3D texture\n");
						writeString(&writer, "#define textureCube texture\n");

						uint32_t fragData = 0;

						if (!!bx::findIdentifierMatch(code, "gl_FragData") )
						{
							for (uint32_t ii = 0, num = g_caps.maxFBAttachments; ii < num; ++ii)
							{
								char tmpFragData[16];
								bx::snprintf(tmpFragData, BX_COUNTOF(tmpFragData), "gl_FragData[%d]", ii);
								fragData = bx::uint32_max(fragData, NULL == strstr(code, tmpFragData) ? 0 : ii+1);
							}

							BGFX_FATAL(0 != fragData, Fatal::InvalidShader, "Unable to find and patch gl_FragData!");
						}

						if (!!bx::findIdentifierMatch(code, "beginFragmentShaderOrdering") )
						{
							if (s_extension[Extension::INTEL_fragment_shader_ordering].m_supported)
							{
								writeString(&writer, "#extension GL_INTEL_fragment_shader_ordering : enable\n");
							}
							else
							{
								writeString(&writer, "#define beginFragmentShaderOrdering()\n");
							}
						}

						if (0 != fragData)
						{
							writeStringf(&writer, "out vec4 bgfx_FragData[%d];\n", fragData);
							writeString(&writer, "#define gl_FragData bgfx_FragData\n");
						}
						else
						{
							writeString(&writer, "out vec4 bgfx_FragColor;\n");
							writeString(&writer, "#define gl_FragColor bgfx_FragColor\n");
						}
					}
					else
					{
						writeString(&writer, "#define attribute in\n");
						writeString(&writer, "#define varying out\n");
					}

					if (!BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 30) )
					{
						writeString(&writer
								, "#define lowp\n"
								  "#define mediump\n"
								  "#define highp\n"
								);
					}

					bx::write(&writer, code, codeLen);
					bx::write(&writer, '\0');
				}

				code = temp;
			}

			GL_CHECK(glShaderSource(m_id, 1, (const GLchar**)&code, NULL) );
			GL_CHECK(glCompileShader(m_id) );

			GLint compiled = 0;
			GL_CHECK(glGetShaderiv(m_id, GL_COMPILE_STATUS, &compiled) );

			if (0 == compiled)
			{
				BX_TRACE("\n####\n%s\n####", code);

				GLsizei len;
				char log[1024];
				GL_CHECK(glGetShaderInfoLog(m_id, sizeof(log), &len, log) );
				BX_TRACE("Failed to compile shader. %d: %s", compiled, log);

				GL_CHECK(glDeleteShader(m_id) );
				m_id = 0;
				BGFX_FATAL(false, bgfx::Fatal::InvalidShader, "Failed to compile shader.");
			}
			else if (BX_ENABLED(BGFX_CONFIG_DEBUG)
				 &&  s_extension[Extension::ANGLE_translated_shader_source].m_supported
				 &&  NULL != glGetTranslatedShaderSourceANGLE)
			{
				GLsizei len;
				GL_CHECK(glGetShaderiv(m_id, GL_TRANSLATED_SHADER_SOURCE_LENGTH_ANGLE, &len) );

				char* source = (char*)alloca(len);
				GL_CHECK(glGetTranslatedShaderSourceANGLE(m_id, len, &len, source) );

				BX_TRACE("ANGLE source (len: %d):\n%s\n####", len, source);
			}
		}
	}

	void ShaderGL::destroy()
	{
		if (0 != m_id)
		{
			GL_CHECK(glDeleteShader(m_id) );
			m_id = 0;
		}
	}

	static void frameBufferValidate()
	{
		GLenum complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		BX_CHECK(GL_FRAMEBUFFER_COMPLETE == complete
			, "glCheckFramebufferStatus failed 0x%08x: %s"
			, complete
			, glEnumName(complete)
		);
		BX_UNUSED(complete);
	}

	void FrameBufferGL::create(uint8_t _num, const TextureHandle* _handles)
	{
		GL_CHECK(glGenFramebuffers(1, &m_fbo[0]) );

		m_numTh = _num;
		memcpy(m_th, _handles, _num*sizeof(TextureHandle) );

		postReset();
	}

	void FrameBufferGL::postReset()
	{
		if (0 != m_fbo[0])
		{
			GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[0]) );

			bool needResolve = false;

			GLenum buffers[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS];

			uint32_t colorIdx = 0;
			for (uint32_t ii = 0; ii < m_numTh; ++ii)
			{
				TextureHandle handle = m_th[ii];
				if (isValid(handle) )
				{
					const TextureGL& texture = s_renderGL->m_textures[handle.idx];

					if (0 == colorIdx)
					{
						m_width  = texture.m_width;
						m_height = texture.m_height;
					}

					GLenum attachment = GL_COLOR_ATTACHMENT0 + colorIdx;
					TextureFormat::Enum format = (TextureFormat::Enum)texture.m_textureFormat;
					if (isDepth(format) )
					{
						const ImageBlockInfo& info = getBlockInfo(format);
						if (0 < info.stencilBits)
						{
							attachment = GL_DEPTH_STENCIL_ATTACHMENT;
						}
						else if (0 == info.depthBits)
						{
							attachment = GL_STENCIL_ATTACHMENT;
						}
						else
						{
							attachment = GL_DEPTH_ATTACHMENT;
						}
					}
					else
					{
						buffers[colorIdx] = attachment;
						++colorIdx;
					}

					if (0 != texture.m_rbo)
					{
						GL_CHECK(glFramebufferRenderbuffer(GL_FRAMEBUFFER
							, attachment
							, GL_RENDERBUFFER
							, texture.m_rbo
							) );
					}
					else
					{
						GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER
							, attachment
							, texture.m_target
							, texture.m_id
							, 0
							) );
					}

					needResolve |= (0 != texture.m_rbo) && (0 != texture.m_id);
				}
			}

			m_num = uint8_t(colorIdx);

			if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL || BGFX_CONFIG_RENDERER_OPENGLES >= 31) )
			{
				if (0 == colorIdx)
				{
					if (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) )
					{
						// When only depth is attached disable draw buffer to avoid
						// GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER.
						GL_CHECK(glDrawBuffer(GL_NONE) );
					}
				}
				else
				{
					GL_CHECK(glDrawBuffers(colorIdx, buffers) );
				}

				// Disable read buffer to avoid GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER.
				GL_CHECK(glReadBuffer(GL_NONE) );
			}

			frameBufferValidate();

			if (needResolve)
			{
				GL_CHECK(glGenFramebuffers(1, &m_fbo[1]) );
				GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]) );

				colorIdx = 0;
				for (uint32_t ii = 0; ii < m_numTh; ++ii)
				{
					TextureHandle handle = m_th[ii];
					if (isValid(handle) )
					{
						const TextureGL& texture = s_renderGL->m_textures[handle.idx];

						if (0 != texture.m_id)
						{
							GLenum attachment = GL_COLOR_ATTACHMENT0 + colorIdx;
							if (!isDepth( (TextureFormat::Enum)texture.m_textureFormat) )
							{
								++colorIdx;
								GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER
									, attachment
									, texture.m_target
									, texture.m_id
									, 0
									) );
							}
						}
					}
				}

				frameBufferValidate();
			}

			GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, s_renderGL->m_msaaBackBufferFbo) );
		}
	}

	void FrameBufferGL::create(uint16_t _denseIdx, void* _nwh, uint32_t _width, uint32_t _height, TextureFormat::Enum _depthFormat)
	{
		BX_UNUSED(_depthFormat);
		m_swapChain = s_renderGL->m_glctx.createSwapChain(_nwh);
		m_width     = _width;
		m_height    = _height;
		m_denseIdx  = _denseIdx;
	}

	uint16_t FrameBufferGL::destroy()
	{
		if (0 != m_num)
		{
			GL_CHECK(glDeleteFramebuffers(0 == m_fbo[1] ? 1 : 2, m_fbo) );
			m_num = 0;
		}

		if (NULL != m_swapChain)
		{
			s_renderGL->m_glctx.destroySwapChain(m_swapChain);
			m_swapChain = NULL;
		}

		memset(m_fbo, 0, sizeof(m_fbo) );
		uint16_t denseIdx = m_denseIdx;
		m_denseIdx = UINT16_MAX;

		return denseIdx;
	}

	void FrameBufferGL::resolve()
	{
		if (0 != m_fbo[1])
		{
			GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo[0]) );
			GL_CHECK(glReadBuffer(GL_COLOR_ATTACHMENT0) );
			GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo[1]) );
			GL_CHECK(glBlitFramebuffer(0
				, 0
				, m_width
				, m_height
				, 0
				, 0
				, m_width
				, m_height
				, GL_COLOR_BUFFER_BIT
				, GL_LINEAR
				) );
			GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo[0]) );
			GL_CHECK(glReadBuffer(GL_NONE) );
			GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, s_renderGL->m_msaaBackBufferFbo) );
		}
	}

	void FrameBufferGL::discard(uint16_t _flags)
	{
		GLenum buffers[BGFX_CONFIG_MAX_FRAME_BUFFER_ATTACHMENTS+2];
		uint32_t idx = 0;

		if (BGFX_CLEAR_NONE != (_flags & BGFX_CLEAR_DISCARD_COLOR_MASK) )
		{
			for (uint32_t ii = 0, num = m_num; ii < num; ++ii)
			{
				if (BGFX_CLEAR_NONE != (_flags & (BGFX_CLEAR_DISCARD_COLOR_0<<ii) ) )
				{
					buffers[idx++] = GL_COLOR_ATTACHMENT0 + ii;
				}
			}
		}

		uint32_t dsFlags = _flags & (BGFX_CLEAR_DISCARD_DEPTH|BGFX_CLEAR_DISCARD_STENCIL);
		if (BGFX_CLEAR_NONE != dsFlags)
		{
			if ( (BGFX_CLEAR_DISCARD_DEPTH|BGFX_CLEAR_DISCARD_STENCIL) == dsFlags)
			{
				buffers[idx++] = GL_DEPTH_STENCIL_ATTACHMENT;
			}
			else if (BGFX_CLEAR_DISCARD_DEPTH == dsFlags)
			{
				buffers[idx++] = GL_DEPTH_ATTACHMENT;
			}
			else if (BGFX_CLEAR_DISCARD_STENCIL == dsFlags)
			{
				buffers[idx++] = GL_STENCIL_ATTACHMENT;
			}
		}

		GL_CHECK(glInvalidateFramebuffer(GL_FRAMEBUFFER, idx, buffers) );
	}

	void RendererContextGL::submit(Frame* _render, ClearQuad& _clearQuad, TextVideoMemBlitter& _textVideoMemBlitter)
	{
		if (1 < m_numWindows
		&&  m_vaoSupport)
		{
			m_vaoSupport = false;
			GL_CHECK(glBindVertexArray(0) );
			GL_CHECK(glDeleteVertexArrays(1, &m_vao) );
			m_vao = 0;
			m_vaoStateCache.invalidate();
		}

		m_glctx.makeCurrent(NULL);

		const GLuint defaultVao = m_vao;
		if (0 != defaultVao)
		{
			GL_CHECK(glBindVertexArray(defaultVao) );
		}

		GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_backBufferFbo) );

		updateResolution(_render->m_resolution);

		int64_t elapsed = -bx::getHPCounter();
		int64_t captureElapsed = 0;

		if (m_timerQuerySupport)
		{
			m_gpuTimer.begin();
		}

		if (0 < _render->m_iboffset)
		{
			TransientIndexBuffer* ib = _render->m_transientIb;
			m_indexBuffers[ib->handle.idx].update(0, _render->m_iboffset, ib->data);
		}

		if (0 < _render->m_vboffset)
		{
			TransientVertexBuffer* vb = _render->m_transientVb;
			m_vertexBuffers[vb->handle.idx].update(0, _render->m_vboffset, vb->data);
		}

		_render->sort();

		RenderDraw currentState;
		currentState.clear();
		currentState.m_flags = BGFX_STATE_NONE;
		currentState.m_stencil = packStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);

		_render->m_hmdInitialized = m_ovr.isInitialized();

		const bool hmdEnabled = m_ovr.isEnabled() || m_ovr.isDebug();
		ViewState viewState(_render, hmdEnabled);

		uint16_t programIdx = invalidHandle;
		SortKey key;
		uint16_t view = UINT16_MAX;
		FrameBufferHandle fbh = BGFX_INVALID_HANDLE;
		int32_t height = hmdEnabled
					? _render->m_hmd.height
					: _render->m_resolution.m_height
					;
		uint32_t blendFactor = 0;

		uint8_t primIndex;
		{
			const uint64_t pt = _render->m_debug&BGFX_DEBUG_WIREFRAME ? BGFX_STATE_PT_LINES : 0;
			primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
		}
		PrimInfo prim = s_primInfo[primIndex];

		uint32_t baseVertex = 0;
		GLuint currentVao = 0;
		bool wasCompute = false;
		bool viewHasScissor = false;
		Rect viewScissorRect;
		viewScissorRect.clear();
		uint16_t discardFlags = BGFX_CLEAR_NONE;

		const bool blendIndependentSupported = s_extension[Extension::ARB_draw_buffers_blend].m_supported;
		const bool computeSupported = (BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL) && s_extension[Extension::ARB_compute_shader].m_supported)
									|| BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGLES >= 31)
									;

		uint32_t statsNumPrimsSubmitted[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumPrimsRendered[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumInstances[BX_COUNTOF(s_primInfo)] = {};
		uint32_t statsNumIndices = 0;
		uint32_t statsKeyType[2] = {};

		if (0 == (_render->m_debug&BGFX_DEBUG_IFH) )
		{
			GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, m_msaaBackBufferFbo) );

			bool viewRestart = false;
			uint8_t eye = 0;
			uint8_t restartState = 0;
			viewState.m_rect = _render->m_rect[0];

			int32_t numItems = _render->m_num;
			for (int32_t item = 0, restartItem = numItems; item < numItems || restartItem < numItems;)
			{
				const bool isCompute   = key.decode(_render->m_sortKeys[item], _render->m_viewRemap);
				statsKeyType[isCompute]++;

				const bool viewChanged = 0
					|| key.m_view != view
					|| item == numItems
					;

				const RenderItem& renderItem = _render->m_renderItem[_render->m_sortValues[item] ];
				++item;

				if (viewChanged)
				{
					if (1 == restartState)
					{
						restartState = 2;
						item = restartItem;
						restartItem = numItems;
						view = UINT16_MAX;
						continue;
					}

					view = key.m_view;
					programIdx = invalidHandle;

					if (_render->m_fb[view].idx != fbh.idx)
					{
						fbh = _render->m_fb[view];
						height = hmdEnabled
							? _render->m_hmd.height
							: _render->m_resolution.m_height
							;
						height = setFrameBuffer(fbh, height, discardFlags);
					}

					viewRestart = ( (BGFX_VIEW_STEREO == (_render->m_viewFlags[view] & BGFX_VIEW_STEREO) ) );
					viewRestart &= hmdEnabled;
					if (viewRestart)
					{
						if (0 == restartState)
						{
							restartState = 1;
							restartItem  = item - 1;
						}

						eye = (restartState - 1) & 1;
						restartState &= 1;
					}
					else
					{
						eye = 0;
					}

					viewState.m_rect = _render->m_rect[view];
					if (viewRestart)
					{
						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
						{
							char* viewName = s_viewName[view];
							viewName[3] = ' ';
							viewName[4] = eye ? 'R' : 'L';
							GL_CHECK(glInsertEventMarker(0, viewName) );
						}

						if (m_ovr.isEnabled() )
						{
							m_ovr.getViewport(eye, &viewState.m_rect);
						}
						else
						{
							viewState.m_rect.m_x = eye * (viewState.m_rect.m_width+1)/2;
							viewState.m_rect.m_width /= 2;
						}
					}
					else
					{
						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
						{
							char* viewName = s_viewName[view];
							viewName[3] = ' ';
							viewName[4] = ' ';
							GL_CHECK(glInsertEventMarker(0, viewName) );
						}
					}

					const Rect& scissorRect = _render->m_scissor[view];
					viewHasScissor = !scissorRect.isZero();
					viewScissorRect = viewHasScissor ? scissorRect : viewState.m_rect;

					GL_CHECK(glViewport(viewState.m_rect.m_x
						, height-viewState.m_rect.m_height-viewState.m_rect.m_y
						, viewState.m_rect.m_width
						, viewState.m_rect.m_height
						) );

					Clear& clear = _render->m_clear[view];
					discardFlags = clear.m_flags & BGFX_CLEAR_DISCARD_MASK;

					if (BGFX_CLEAR_NONE != (clear.m_flags & BGFX_CLEAR_MASK) )
					{
						clearQuad(_clearQuad, viewState.m_rect, clear, height, _render->m_clearColor);
					}

					GL_CHECK(glDisable(GL_STENCIL_TEST) );
					GL_CHECK(glEnable(GL_DEPTH_TEST) );
					GL_CHECK(glDepthFunc(GL_LESS) );
					GL_CHECK(glEnable(GL_CULL_FACE) );
					GL_CHECK(glDisable(GL_BLEND) );
				}

				if (isCompute)
				{
					if (!wasCompute)
					{
						wasCompute = true;

						if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
						{
							char* viewName = s_viewName[view];
							viewName[3] = 'C';
							GL_CHECK(glInsertEventMarker(0, viewName) );
						}
					}

					if (computeSupported)
					{
						const RenderCompute& compute = renderItem.compute;

						ProgramGL& program = m_program[key.m_program];
 						GL_CHECK(glUseProgram(program.m_id) );

						GLbitfield barrier = 0;
						for (uint32_t ii = 0; ii < BGFX_MAX_COMPUTE_BINDINGS; ++ii)
						{
							const Binding& bind = compute.m_bind[ii];
							if (invalidHandle != bind.m_idx)
							{
								switch (bind.m_type)
								{
								case Binding::Image:
									{
										const TextureGL& texture = m_textures[bind.m_idx];
										GL_CHECK(glBindImageTexture(ii
											, texture.m_id
											, bind.m_un.m_compute.m_mip
											, GL_FALSE
											, 0
											, s_access[bind.m_un.m_compute.m_access]
											, s_imageFormat[bind.m_un.m_compute.m_format])
											);
										barrier |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
									}
									break;

								case Binding::IndexBuffer:
									{
										const IndexBufferGL& buffer = m_indexBuffers[bind.m_idx];
										GL_CHECK(glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ii, buffer.m_id) );
										barrier |= GL_SHADER_STORAGE_BARRIER_BIT;
									}
									break;

								case Binding::VertexBuffer:
									{
										const VertexBufferGL& buffer = m_vertexBuffers[bind.m_idx];
										GL_CHECK(glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ii, buffer.m_id) );
										barrier |= GL_SHADER_STORAGE_BARRIER_BIT;
									}
									break;
								}
							}
						}

						if (0 != barrier)
						{
							bool constantsChanged = compute.m_constBegin < compute.m_constEnd;
							rendererUpdateUniforms(this, _render->m_constantBuffer, compute.m_constBegin, compute.m_constEnd);

							if (constantsChanged
							&&  NULL != program.m_constantBuffer)
							{
								commit(*program.m_constantBuffer);
							}

							viewState.setPredefined<1>(this, view, eye, program, _render, compute);

							if (isValid(compute.m_indirectBuffer) )
							{
								const VertexBufferGL& vb = m_vertexBuffers[compute.m_indirectBuffer.idx];
								if (currentState.m_indirectBuffer.idx != compute.m_indirectBuffer.idx)
								{
									currentState.m_indirectBuffer = compute.m_indirectBuffer;
									GL_CHECK(glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, vb.m_id) );
								}

								uint32_t numDrawIndirect = UINT16_MAX == compute.m_numIndirect
									? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
									: compute.m_numIndirect
									;

								uintptr_t args = compute.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
								for (uint32_t ii = 0; ii < numDrawIndirect; ++ii)
								{
									GL_CHECK(glDispatchComputeIndirect((GLintptr)args) );
									args += BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
								}
							}
							else
							{
								if (isValid(currentState.m_indirectBuffer) )
								{
									currentState.m_indirectBuffer.idx = invalidHandle;
									GL_CHECK(glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0) );
								}

								GL_CHECK(glDispatchCompute(compute.m_numX, compute.m_numY, compute.m_numZ) );
							}

							GL_CHECK(glMemoryBarrier(barrier) );
						}
					}

					continue;
				}

				bool resetState = viewChanged || wasCompute;

				if (wasCompute)
				{
					wasCompute = false;

					if (BX_ENABLED(BGFX_CONFIG_DEBUG_PIX) )
					{
						char* viewName = s_viewName[view];
						viewName[3] = ' ';
						GL_CHECK(glInsertEventMarker(0, viewName) );
					}
				}

				const RenderDraw& draw = renderItem.draw;

				const uint64_t newFlags = draw.m_flags;
				uint64_t changedFlags = currentState.m_flags ^ draw.m_flags;
				currentState.m_flags = newFlags;

				const uint64_t newStencil = draw.m_stencil;
				uint64_t changedStencil = currentState.m_stencil ^ draw.m_stencil;
				currentState.m_stencil = newStencil;

				if (resetState)
				{
					currentState.clear();
					currentState.m_scissor = !draw.m_scissor;
					changedFlags = BGFX_STATE_MASK;
					changedStencil = packStencil(BGFX_STENCIL_MASK, BGFX_STENCIL_MASK);
					currentState.m_flags = newFlags;
					currentState.m_stencil = newStencil;
				}

				uint16_t scissor = draw.m_scissor;
				if (currentState.m_scissor != scissor)
				{
					currentState.m_scissor = scissor;

					if (UINT16_MAX == scissor)
					{
						if (viewHasScissor)
						{
							GL_CHECK(glEnable(GL_SCISSOR_TEST) );
							GL_CHECK(glScissor(viewScissorRect.m_x
								, height-viewScissorRect.m_height-viewScissorRect.m_y
								, viewScissorRect.m_width
								, viewScissorRect.m_height
								) );
						}
						else
						{
							GL_CHECK(glDisable(GL_SCISSOR_TEST) );
						}
					}
					else
					{
						Rect scissorRect;
						scissorRect.intersect(viewScissorRect, _render->m_rectCache.m_cache[scissor]);
						GL_CHECK(glEnable(GL_SCISSOR_TEST) );
						GL_CHECK(glScissor(scissorRect.m_x
							, height-scissorRect.m_height-scissorRect.m_y
							, scissorRect.m_width
							, scissorRect.m_height
							) );
					}
				}

				if (0 != changedStencil)
				{
					if (0 != newStencil)
					{
						GL_CHECK(glEnable(GL_STENCIL_TEST) );

						uint32_t bstencil = unpackStencil(1, newStencil);
						uint8_t frontAndBack = bstencil != BGFX_STENCIL_NONE && bstencil != unpackStencil(0, newStencil);

// 						uint32_t bchanged = unpackStencil(1, changedStencil);
// 						if (BGFX_STENCIL_FUNC_RMASK_MASK & bchanged)
// 						{
// 							uint32_t wmask = (bstencil&BGFX_STENCIL_FUNC_RMASK_MASK)>>BGFX_STENCIL_FUNC_RMASK_SHIFT;
// 							GL_CHECK(glStencilMask(wmask) );
// 						}

						for (uint8_t ii = 0, num = frontAndBack+1; ii < num; ++ii)
						{
							uint32_t stencil = unpackStencil(ii, newStencil);
							uint32_t changed = unpackStencil(ii, changedStencil);
							GLenum face = s_stencilFace[frontAndBack+ii];

							if ( (BGFX_STENCIL_TEST_MASK|BGFX_STENCIL_FUNC_REF_MASK|BGFX_STENCIL_FUNC_RMASK_MASK) & changed)
							{
								GLint ref = (stencil&BGFX_STENCIL_FUNC_REF_MASK)>>BGFX_STENCIL_FUNC_REF_SHIFT;
								GLint mask = (stencil&BGFX_STENCIL_FUNC_RMASK_MASK)>>BGFX_STENCIL_FUNC_RMASK_SHIFT;
								uint32_t func = (stencil&BGFX_STENCIL_TEST_MASK)>>BGFX_STENCIL_TEST_SHIFT;
								GL_CHECK(glStencilFuncSeparate(face, s_cmpFunc[func], ref, mask) );
							}

							if ( (BGFX_STENCIL_OP_FAIL_S_MASK|BGFX_STENCIL_OP_FAIL_Z_MASK|BGFX_STENCIL_OP_PASS_Z_MASK) & changed)
							{
								uint32_t sfail = (stencil&BGFX_STENCIL_OP_FAIL_S_MASK)>>BGFX_STENCIL_OP_FAIL_S_SHIFT;
								uint32_t zfail = (stencil&BGFX_STENCIL_OP_FAIL_Z_MASK)>>BGFX_STENCIL_OP_FAIL_Z_SHIFT;
								uint32_t zpass = (stencil&BGFX_STENCIL_OP_PASS_Z_MASK)>>BGFX_STENCIL_OP_PASS_Z_SHIFT;
								GL_CHECK(glStencilOpSeparate(face, s_stencilOp[sfail], s_stencilOp[zfail], s_stencilOp[zpass]) );
							}
						}
					}
					else
					{
						GL_CHECK(glDisable(GL_STENCIL_TEST) );
					}
				}

				if ( (0
					 | BGFX_STATE_CULL_MASK
					 | BGFX_STATE_DEPTH_WRITE
					 | BGFX_STATE_DEPTH_TEST_MASK
					 | BGFX_STATE_RGB_WRITE
					 | BGFX_STATE_ALPHA_WRITE
					 | BGFX_STATE_BLEND_MASK
					 | BGFX_STATE_BLEND_EQUATION_MASK
					 | BGFX_STATE_ALPHA_REF_MASK
					 | BGFX_STATE_PT_MASK
					 | BGFX_STATE_POINT_SIZE_MASK
					 | BGFX_STATE_MSAA
					 ) & changedFlags)
				{
					if (BGFX_STATE_CULL_MASK & changedFlags)
					{
						if (BGFX_STATE_CULL_CW & newFlags)
						{
							GL_CHECK(glEnable(GL_CULL_FACE) );
							GL_CHECK(glCullFace(GL_BACK) );
						}
						else if (BGFX_STATE_CULL_CCW & newFlags)
						{
							GL_CHECK(glEnable(GL_CULL_FACE) );
							GL_CHECK(glCullFace(GL_FRONT) );
						}
						else
						{
							GL_CHECK(glDisable(GL_CULL_FACE) );
						}
					}

					if (BGFX_STATE_DEPTH_WRITE & changedFlags)
					{
						GL_CHECK(glDepthMask(!!(BGFX_STATE_DEPTH_WRITE & newFlags) ) );
					}

					if (BGFX_STATE_DEPTH_TEST_MASK & changedFlags)
					{
						uint32_t func = (newFlags&BGFX_STATE_DEPTH_TEST_MASK)>>BGFX_STATE_DEPTH_TEST_SHIFT;

						if (0 != func)
						{
							GL_CHECK(glEnable(GL_DEPTH_TEST) );
							GL_CHECK(glDepthFunc(s_cmpFunc[func]) );
						}
						else
						{
							GL_CHECK(glDisable(GL_DEPTH_TEST) );
						}
					}

					if (BGFX_STATE_ALPHA_REF_MASK & changedFlags)
					{
						uint32_t ref = (newFlags&BGFX_STATE_ALPHA_REF_MASK)>>BGFX_STATE_ALPHA_REF_SHIFT;
						viewState.m_alphaRef = ref/255.0f;
					}

#if BGFX_CONFIG_RENDERER_OPENGL
					if ( (BGFX_STATE_PT_POINTS|BGFX_STATE_POINT_SIZE_MASK) & changedFlags)
					{
						float pointSize = (float)(bx::uint32_max(1, (newFlags&BGFX_STATE_POINT_SIZE_MASK)>>BGFX_STATE_POINT_SIZE_SHIFT) );
						GL_CHECK(glPointSize(pointSize) );
					}

					if (BGFX_STATE_MSAA & changedFlags)
					{
						if (BGFX_STATE_MSAA & newFlags)
						{
							GL_CHECK(glEnable(GL_MULTISAMPLE) );
						}
						else
						{
							GL_CHECK(glDisable(GL_MULTISAMPLE) );
						}
					}
#endif // BGFX_CONFIG_RENDERER_OPENGL

					if ( (BGFX_STATE_ALPHA_WRITE|BGFX_STATE_RGB_WRITE) & changedFlags)
					{
						GLboolean alpha = !!(newFlags&BGFX_STATE_ALPHA_WRITE);
						GLboolean rgb = !!(newFlags&BGFX_STATE_RGB_WRITE);
						GL_CHECK(glColorMask(rgb, rgb, rgb, alpha) );
					}

					if ( (BGFX_STATE_BLEND_MASK|BGFX_STATE_BLEND_EQUATION_MASK|BGFX_STATE_BLEND_INDEPENDENT) & changedFlags
					||  blendFactor != draw.m_rgba)
					{
						if ( (BGFX_STATE_BLEND_MASK|BGFX_STATE_BLEND_EQUATION_MASK|BGFX_STATE_BLEND_INDEPENDENT) & newFlags
						||  blendFactor != draw.m_rgba)
						{
							const bool enabled = !!(BGFX_STATE_BLEND_MASK & newFlags);
							const bool independent = !!(BGFX_STATE_BLEND_INDEPENDENT & newFlags)
								&& blendIndependentSupported
								;

							const uint32_t blend  = uint32_t( (newFlags&BGFX_STATE_BLEND_MASK)>>BGFX_STATE_BLEND_SHIFT);
							const uint32_t srcRGB = (blend    )&0xf;
							const uint32_t dstRGB = (blend>> 4)&0xf;
							const uint32_t srcA   = (blend>> 8)&0xf;
							const uint32_t dstA   = (blend>>12)&0xf;

							const uint32_t equ    = uint32_t( (newFlags&BGFX_STATE_BLEND_EQUATION_MASK)>>BGFX_STATE_BLEND_EQUATION_SHIFT);
							const uint32_t equRGB = (equ   )&0x7;
							const uint32_t equA   = (equ>>3)&0x7;

							const uint32_t numRt = getNumRt();

							if (!BX_ENABLED(BGFX_CONFIG_RENDERER_OPENGL)
							||  1 >= numRt
							||  !independent)
							{
								if (enabled)
								{
									GL_CHECK(glEnable(GL_BLEND) );
									GL_CHECK(glBlendFuncSeparate(s_blendFactor[srcRGB].m_src
										, s_blendFactor[dstRGB].m_dst
										, s_blendFactor[srcA].m_src
										, s_blendFactor[dstA].m_dst
										) );
									GL_CHECK(glBlendEquationSeparate(s_blendEquation[equRGB], s_blendEquation[equA]) );

									if ( (s_blendFactor[srcRGB].m_factor || s_blendFactor[dstRGB].m_factor)
									&&  blendFactor != draw.m_rgba)
									{
										const uint32_t rgba = draw.m_rgba;
										GLclampf rr = ( (rgba>>24)     )/255.0f;
										GLclampf gg = ( (rgba>>16)&0xff)/255.0f;
										GLclampf bb = ( (rgba>> 8)&0xff)/255.0f;
										GLclampf aa = ( (rgba    )&0xff)/255.0f;

										GL_CHECK(glBlendColor(rr, gg, bb, aa) );
									}
								}
								else
								{
									GL_CHECK(glDisable(GL_BLEND) );
								}
							}
							else
							{
								if (enabled)
								{
									GL_CHECK(glEnablei(GL_BLEND, 0) );
									GL_CHECK(glBlendFuncSeparatei(0
										, s_blendFactor[srcRGB].m_src
										, s_blendFactor[dstRGB].m_dst
										, s_blendFactor[srcA].m_src
										, s_blendFactor[dstA].m_dst
										) );
									GL_CHECK(glBlendEquationSeparatei(0
										, s_blendEquation[equRGB]
										, s_blendEquation[equA]
										) );
								}
								else
								{
									GL_CHECK(glDisablei(GL_BLEND, 0) );
								}

								for (uint32_t ii = 1, rgba = draw.m_rgba; ii < numRt; ++ii, rgba >>= 11)
								{
									if (0 != (rgba&0x7ff) )
									{
										const uint32_t src      = (rgba   )&0xf;
										const uint32_t dst      = (rgba>>4)&0xf;
										const uint32_t equation = (rgba>>8)&0x7;
										GL_CHECK(glEnablei(GL_BLEND, ii) );
										GL_CHECK(glBlendFunci(ii, s_blendFactor[src].m_src, s_blendFactor[dst].m_dst) );
										GL_CHECK(glBlendEquationi(ii, s_blendEquation[equation]) );
									}
									else
									{
										GL_CHECK(glDisablei(GL_BLEND, ii) );
									}
								}
							}
						}
						else
						{
							GL_CHECK(glDisable(GL_BLEND) );
						}

						blendFactor = draw.m_rgba;
					}

					const uint64_t pt = _render->m_debug&BGFX_DEBUG_WIREFRAME ? BGFX_STATE_PT_LINES : newFlags&BGFX_STATE_PT_MASK;
					primIndex = uint8_t(pt>>BGFX_STATE_PT_SHIFT);
					prim = s_primInfo[primIndex];
				}

				bool programChanged = false;
				bool constantsChanged = draw.m_constBegin < draw.m_constEnd;
				bool bindAttribs = false;
				rendererUpdateUniforms(this, _render->m_constantBuffer, draw.m_constBegin, draw.m_constEnd);

				if (key.m_program != programIdx)
				{
					programIdx = key.m_program;
					GLuint id = invalidHandle == programIdx ? 0 : m_program[programIdx].m_id;

					// Skip rendering if program index is valid, but program is invalid.
					programIdx = 0 == id ? invalidHandle : programIdx;

					GL_CHECK(glUseProgram(id) );
					programChanged =
						constantsChanged =
						bindAttribs = true;
				}

				if (invalidHandle != programIdx)
				{
					ProgramGL& program = m_program[programIdx];

					if (constantsChanged
					&&  NULL != program.m_constantBuffer)
					{
						commit(*program.m_constantBuffer);
					}

					viewState.setPredefined<1>(this, view, eye, program, _render, draw);

					{
						for (uint32_t stage = 0; stage < BGFX_CONFIG_MAX_TEXTURE_SAMPLERS; ++stage)
						{
							const Binding& bind = draw.m_bind[stage];
							Binding& current = currentState.m_bind[stage];
							if (current.m_idx != bind.m_idx
							||  current.m_un.m_draw.m_flags != bind.m_un.m_draw.m_flags
							||  programChanged)
							{
								if (invalidHandle != bind.m_idx)
								{
									TextureGL& texture = m_textures[bind.m_idx];
									texture.commit(stage, bind.m_un.m_draw.m_flags);
								}
							}

							current = bind;
						}
					}

					if (0 != defaultVao
					&&  0 == draw.m_startVertex
					&&  0 == draw.m_instanceDataOffset)
					{
						if (programChanged
						||  baseVertex                        != draw.m_startVertex
						||  currentState.m_vertexBuffer.idx   != draw.m_vertexBuffer.idx
						||  currentState.m_indexBuffer.idx    != draw.m_indexBuffer.idx
						||  currentState.m_instanceDataOffset != draw.m_instanceDataOffset
						||  currentState.m_instanceDataStride != draw.m_instanceDataStride
						||  currentState.m_instanceDataBuffer.idx != draw.m_instanceDataBuffer.idx)
						{
							bx::HashMurmur2A murmur;
							murmur.begin();
							murmur.add(draw.m_vertexBuffer.idx);

							if (isValid(draw.m_vertexBuffer) )
							{
								const VertexBufferGL& vb = m_vertexBuffers[draw.m_vertexBuffer.idx];
								uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
								murmur.add(decl);
							}

							murmur.add(draw.m_indexBuffer.idx);
							murmur.add(draw.m_instanceDataBuffer.idx);
							murmur.add(draw.m_instanceDataOffset);
							murmur.add(draw.m_instanceDataStride);
							murmur.add(programIdx);
							uint32_t hash = murmur.end();

							currentState.m_vertexBuffer = draw.m_vertexBuffer;
							currentState.m_indexBuffer = draw.m_indexBuffer;
							currentState.m_instanceDataOffset = draw.m_instanceDataOffset;
							currentState.m_instanceDataStride = draw.m_instanceDataStride;
							baseVertex = draw.m_startVertex;

							GLuint id = m_vaoStateCache.find(hash);
							if (UINT32_MAX != id)
							{
								currentVao = id;
								GL_CHECK(glBindVertexArray(id) );
							}
							else
							{
								id = m_vaoStateCache.add(hash);
								currentVao = id;
								GL_CHECK(glBindVertexArray(id) );

								program.add(hash);

								if (isValid(draw.m_vertexBuffer) )
								{
									VertexBufferGL& vb = m_vertexBuffers[draw.m_vertexBuffer.idx];
									vb.add(hash);
									GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vb.m_id) );

									uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
									program.bindAttributes(m_vertexDecls[decl], draw.m_startVertex);

									if (isValid(draw.m_instanceDataBuffer) )
									{
										VertexBufferGL& instanceVb = m_vertexBuffers[draw.m_instanceDataBuffer.idx];
										instanceVb.add(hash);
										GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, instanceVb.m_id) );
										program.bindInstanceData(draw.m_instanceDataStride, draw.m_instanceDataOffset);
									}
								}
								else
								{
									GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0) );
								}

								if (isValid(draw.m_indexBuffer) )
								{
									IndexBufferGL& ib = m_indexBuffers[draw.m_indexBuffer.idx];
									ib.add(hash);
									GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib.m_id) );
								}
								else
								{
									GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0) );
								}
							}
						}
					}
					else
					{
						if (0 != defaultVao
						&&  0 != currentVao)
						{
							GL_CHECK(glBindVertexArray(defaultVao) );
							currentState.m_vertexBuffer.idx = invalidHandle;
							currentState.m_indexBuffer.idx = invalidHandle;
							bindAttribs = true;
							currentVao = 0;
						}

						if (programChanged
						||  currentState.m_vertexBuffer.idx != draw.m_vertexBuffer.idx
						||  currentState.m_instanceDataBuffer.idx != draw.m_instanceDataBuffer.idx
						||  currentState.m_instanceDataOffset != draw.m_instanceDataOffset
						||  currentState.m_instanceDataStride != draw.m_instanceDataStride)
						{
							currentState.m_vertexBuffer = draw.m_vertexBuffer;
							currentState.m_instanceDataBuffer.idx = draw.m_instanceDataBuffer.idx;
							currentState.m_instanceDataOffset = draw.m_instanceDataOffset;
							currentState.m_instanceDataStride = draw.m_instanceDataStride;

							uint16_t handle = draw.m_vertexBuffer.idx;
							if (invalidHandle != handle)
							{
								VertexBufferGL& vb = m_vertexBuffers[handle];
								GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vb.m_id) );
								bindAttribs = true;
							}
							else
							{
								GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0) );
							}
						}

						if (currentState.m_indexBuffer.idx != draw.m_indexBuffer.idx)
						{
							currentState.m_indexBuffer = draw.m_indexBuffer;

							uint16_t handle = draw.m_indexBuffer.idx;
							if (invalidHandle != handle)
							{
								IndexBufferGL& ib = m_indexBuffers[handle];
								GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib.m_id) );
							}
							else
							{
								GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0) );
							}
						}

						if (isValid(currentState.m_vertexBuffer) )
						{
							if (baseVertex != draw.m_startVertex
							||  bindAttribs)
							{
								baseVertex = draw.m_startVertex;
								const VertexBufferGL& vb = m_vertexBuffers[draw.m_vertexBuffer.idx];
								uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
								program.bindAttributes(m_vertexDecls[decl], draw.m_startVertex);

								if (isValid(draw.m_instanceDataBuffer) )
								{
									GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffers[draw.m_instanceDataBuffer.idx].m_id) );
									program.bindInstanceData(draw.m_instanceDataStride, draw.m_instanceDataOffset);
								}
							}
						}
					}

					if (isValid(currentState.m_vertexBuffer) )
					{
						uint32_t numVertices = draw.m_numVertices;
						if (UINT32_MAX == numVertices)
						{
							const VertexBufferGL& vb = m_vertexBuffers[currentState.m_vertexBuffer.idx];
							uint16_t decl = !isValid(vb.m_decl) ? draw.m_vertexDecl.idx : vb.m_decl.idx;
							const VertexDecl& vertexDecl = m_vertexDecls[decl];
							numVertices = vb.m_size/vertexDecl.m_stride;
						}

						uint32_t numIndices        = 0;
						uint32_t numPrimsSubmitted = 0;
						uint32_t numInstances      = 0;
						uint32_t numPrimsRendered  = 0;
						uint32_t numDrawIndirect   = 0;

						if (isValid(draw.m_indirectBuffer) )
						{
							const VertexBufferGL& vb = m_vertexBuffers[draw.m_indirectBuffer.idx];
							if (currentState.m_indirectBuffer.idx != draw.m_indirectBuffer.idx)
							{
								currentState.m_indirectBuffer = draw.m_indirectBuffer;
								GL_CHECK(glBindBuffer(GL_DRAW_INDIRECT_BUFFER, vb.m_id) );
							}

							if (isValid(draw.m_indexBuffer) )
							{
								const IndexBufferGL& ib = m_indexBuffers[draw.m_indexBuffer.idx];
								const bool hasIndex16 = 0 == (ib.m_flags & BGFX_BUFFER_INDEX32);
								const GLenum indexFormat = hasIndex16
									? GL_UNSIGNED_SHORT
									: GL_UNSIGNED_INT
									;

								numDrawIndirect = UINT16_MAX == draw.m_numIndirect
									? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
									: draw.m_numIndirect
									;

								uintptr_t args = draw.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
								GL_CHECK(glMultiDrawElementsIndirect(prim.m_type, indexFormat
									, (void*)args
									, numDrawIndirect
									, BGFX_CONFIG_DRAW_INDIRECT_STRIDE
									) );
							}
							else
							{
								numDrawIndirect = UINT16_MAX == draw.m_numIndirect
									? vb.m_size/BGFX_CONFIG_DRAW_INDIRECT_STRIDE
									: draw.m_numIndirect
									;

								uintptr_t args = draw.m_startIndirect * BGFX_CONFIG_DRAW_INDIRECT_STRIDE;
								GL_CHECK(glMultiDrawArraysIndirect(prim.m_type
									, (void*)args
									, numDrawIndirect
									, BGFX_CONFIG_DRAW_INDIRECT_STRIDE
									) );
							}
						}
						else
						{
							if (isValid(currentState.m_indirectBuffer) )
							{
								currentState.m_indirectBuffer.idx = invalidHandle;
								GL_CHECK(glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0) );
							}

							if (isValid(draw.m_indexBuffer) )
							{
								const IndexBufferGL& ib = m_indexBuffers[draw.m_indexBuffer.idx];
								const bool hasIndex16 = 0 == (ib.m_flags & BGFX_BUFFER_INDEX32);
								const uint32_t indexSize = hasIndex16 ? 2 : 4;
								const GLenum indexFormat = hasIndex16
									? GL_UNSIGNED_SHORT
									: GL_UNSIGNED_INT
									;

								if (UINT32_MAX == draw.m_numIndices)
								{
									numIndices        = ib.m_size/indexSize;
									numPrimsSubmitted = numIndices/prim.m_div - prim.m_sub;
									numInstances      = draw.m_numInstances;
									numPrimsRendered  = numPrimsSubmitted*draw.m_numInstances;

									GL_CHECK(glDrawElementsInstanced(prim.m_type
										, numIndices
										, indexFormat
										, (void*)0
										, draw.m_numInstances
										) );
								}
								else if (prim.m_min <= draw.m_numIndices)
								{
									numIndices = draw.m_numIndices;
									numPrimsSubmitted = numIndices/prim.m_div - prim.m_sub;
									numInstances = draw.m_numInstances;
									numPrimsRendered = numPrimsSubmitted*draw.m_numInstances;

									GL_CHECK(glDrawElementsInstanced(prim.m_type
										, numIndices
										, indexFormat
										, (void*)(uintptr_t)(draw.m_startIndex*indexSize)
										, draw.m_numInstances
										) );
								}
							}
							else
							{
								numPrimsSubmitted = numVertices/prim.m_div - prim.m_sub;
								numInstances = draw.m_numInstances;
								numPrimsRendered = numPrimsSubmitted*draw.m_numInstances;

								GL_CHECK(glDrawArraysInstanced(prim.m_type
									, 0
									, numVertices
									, draw.m_numInstances
									) );
							}
						}

						statsNumPrimsSubmitted[primIndex] += numPrimsSubmitted;
						statsNumPrimsRendered[primIndex]  += numPrimsRendered;
						statsNumInstances[primIndex]      += numInstances;
						statsNumIndices += numIndices;
					}
				}
			}

			blitMsaaFbo();

			if (0 < _render->m_num)
			{
				if (0 != (m_resolution.m_flags & BGFX_RESET_FLUSH_AFTER_RENDER) )
				{
					GL_CHECK(glFlush() );
				}

				captureElapsed = -bx::getHPCounter();
				capture();
				captureElapsed += bx::getHPCounter();
			}
		}

		m_glctx.makeCurrent(NULL);
		int64_t now = bx::getHPCounter();
		elapsed += now;

		static int64_t last = now;
		int64_t frameTime = now - last;
		last = now;

		static int64_t min = frameTime;
		static int64_t max = frameTime;
		min = min > frameTime ? frameTime : min;
		max = max < frameTime ? frameTime : max;

		static uint32_t maxGpuLatency = 0;
		static double   maxGpuElapsed = 0.0f;
		double elapsedGpuMs = 0.0;
		uint64_t elapsedGl  = 0;

		if (m_timerQuerySupport)
		{
			m_gpuTimer.end();
			while (m_gpuTimer.get() )
			{
				elapsedGl     = m_gpuTimer.m_elapsed;
				elapsedGpuMs  = double(elapsedGl)/1e6;
				maxGpuElapsed = elapsedGpuMs > maxGpuElapsed ? elapsedGpuMs : maxGpuElapsed;
			}
			maxGpuLatency = bx::uint32_imax(maxGpuLatency, m_gpuTimer.m_control.available()-1);
		}

		const int64_t timerFreq = bx::getHPFrequency();

		Stats& perfStats   = _render->m_perfStats;
		perfStats.cpuTime      = frameTime;
		perfStats.cpuTimerFreq = timerFreq;
		perfStats.gpuTime      = elapsedGl;
		perfStats.gpuTimerFreq = 100000000;

		if (_render->m_debug & (BGFX_DEBUG_IFH|BGFX_DEBUG_STATS) )
		{
			TextVideoMem& tvm = m_textVideoMem;

			static int64_t next = now;

			if (now >= next)
			{
				next = now + timerFreq;
				double freq = double(timerFreq);
				double toMs = 1000.0/freq;

				tvm.clear();
				uint16_t pos = 0;
				tvm.printf(0, pos++, BGFX_CONFIG_DEBUG ? 0x89 : 0x8f, " %s / " BX_COMPILER_NAME " / " BX_CPU_NAME " / " BX_ARCH_NAME " / " BX_PLATFORM_NAME " "
					, getRendererName()
					);
				tvm.printf(0, pos++, 0x8f, "       Vendor: %s ", m_vendor);
				tvm.printf(0, pos++, 0x8f, "     Renderer: %s ", m_renderer);
				tvm.printf(0, pos++, 0x8f, "      Version: %s ", m_version);
				tvm.printf(0, pos++, 0x8f, " GLSL version: %s ", m_glslVersion);

				pos = 10;
				tvm.printf(10, pos++, 0x8e, "      Frame CPU: %7.3f, % 7.3f \x1f, % 7.3f \x1e [ms] / % 6.2f FPS "
					, double(frameTime)*toMs
					, double(min)*toMs
					, double(max)*toMs
					, freq/frameTime
					);

				char hmd[16];
				bx::snprintf(hmd, BX_COUNTOF(hmd), ", [%c] HMD ", hmdEnabled ? '\xfe' : ' ');

				const uint32_t msaa = (m_resolution.m_flags&BGFX_RESET_MSAA_MASK)>>BGFX_RESET_MSAA_SHIFT;
				tvm.printf(10, pos++, 0x8e, "    Reset flags: [%c] vsync, [%c] MSAAx%d%s, [%c] MaxAnisotropy "
					, !!(m_resolution.m_flags&BGFX_RESET_VSYNC) ? '\xfe' : ' '
					, 0 != msaa ? '\xfe' : ' '
					, 1<<msaa
					, m_ovr.isInitialized() ? hmd : ", no-HMD "
					, !!(m_resolution.m_flags&BGFX_RESET_MAXANISOTROPY) ? '\xfe' : ' '
					);

				double elapsedCpuMs = double(elapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "   Submitted: %5d (draw %5d, compute %4d) / CPU %7.4f [ms] %c GPU %7.4f [ms] (latency %d) "
					, _render->m_num
					, statsKeyType[0]
					, statsKeyType[1]
					, elapsedCpuMs
					, elapsedCpuMs > elapsedGpuMs ? '>' : '<'
					, maxGpuElapsed
					, maxGpuLatency
					);
				maxGpuLatency = 0;
				maxGpuElapsed = 0.0;

				for (uint32_t ii = 0; ii < BX_COUNTOF(s_primInfo); ++ii)
				{
					tvm.printf(10, pos++, 0x8e, "   %9s: %7d (#inst: %5d), submitted: %7d "
						, s_primName[ii]
						, statsNumPrimsRendered[ii]
						, statsNumInstances[ii]
						, statsNumPrimsSubmitted[ii]
						);
				}

				if (NULL != m_renderdocdll)
				{
					tvm.printf(tvm.m_width-27, 0, 0x1f, " [F11 - RenderDoc capture] ");
				}

				tvm.printf(10, pos++, 0x8e, "      Indices: %7d ", statsNumIndices);
				tvm.printf(10, pos++, 0x8e, " Uniform size: %7d ", _render->m_constEnd);
				tvm.printf(10, pos++, 0x8e, "     DVB size: %7d ", _render->m_vboffset);
				tvm.printf(10, pos++, 0x8e, "     DIB size: %7d ", _render->m_iboffset);

				pos++;
				tvm.printf(10, pos++, 0x8e, " State cache:     ");
				tvm.printf(10, pos++, 0x8e, " VAO    | Sampler ");
				tvm.printf(10, pos++, 0x8e, " %6d | %6d  "
					, m_vaoStateCache.getCount()
					, m_samplerStateCache.getCount()
					);

#if BGFX_CONFIG_RENDERER_OPENGL
				if (s_extension[Extension::ATI_meminfo].m_supported)
				{
					GLint vboFree[4];
					GL_CHECK(glGetIntegerv(GL_VBO_FREE_MEMORY_ATI, vboFree) );

					GLint texFree[4];
					GL_CHECK(glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, texFree) );

					GLint rbfFree[4];
					GL_CHECK(glGetIntegerv(GL_RENDERBUFFER_FREE_MEMORY_ATI, rbfFree) );

					pos++;
					tvm.printf(10, pos++, 0x8c, " -------------|    free|  free b|     aux|  aux fb ");

					char tmp0[16];
					char tmp1[16];
					char tmp2[16];
					char tmp3[16];

					bx::prettify(tmp0, BX_COUNTOF(tmp0), vboFree[0]);
					bx::prettify(tmp1, BX_COUNTOF(tmp1), vboFree[1]);
					bx::prettify(tmp2, BX_COUNTOF(tmp2), vboFree[2]);
					bx::prettify(tmp3, BX_COUNTOF(tmp3), vboFree[3]);
					tvm.printf(10, pos++, 0x8e, "           VBO: %10s, %10s, %10s, %10s ", tmp0, tmp1, tmp2, tmp3);

					bx::prettify(tmp0, BX_COUNTOF(tmp0), texFree[0]);
					bx::prettify(tmp1, BX_COUNTOF(tmp1), texFree[1]);
					bx::prettify(tmp2, BX_COUNTOF(tmp2), texFree[2]);
					bx::prettify(tmp3, BX_COUNTOF(tmp3), texFree[3]);
					tvm.printf(10, pos++, 0x8e, "       Texture: %10s, %10s, %10s, %10s ", tmp0, tmp1, tmp2, tmp3);

					bx::prettify(tmp0, BX_COUNTOF(tmp0), rbfFree[0]);
					bx::prettify(tmp1, BX_COUNTOF(tmp1), rbfFree[1]);
					bx::prettify(tmp2, BX_COUNTOF(tmp2), rbfFree[2]);
					bx::prettify(tmp3, BX_COUNTOF(tmp3), rbfFree[3]);
					tvm.printf(10, pos++, 0x8e, " Render Buffer: %10s, %10s, %10s, %10s ", tmp0, tmp1, tmp2, tmp3);
				}
				else if (s_extension[Extension::NVX_gpu_memory_info].m_supported)
				{
					GLint dedicated;
					GL_CHECK(glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicated) );

					GLint totalAvail;
					GL_CHECK(glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &totalAvail) );
					GLint currAvail;
					GL_CHECK(glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &currAvail) );

					GLint evictedCount;
					GL_CHECK(glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &evictedCount) );

					GLint evictedMemory;
					GL_CHECK(glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX, &evictedMemory) );

					pos++;

					char tmp0[16];
					char tmp1[16];

					bx::prettify(tmp0, BX_COUNTOF(tmp0), dedicated);
					tvm.printf(10, pos++, 0x8e, " Dedicated: %10s ", tmp0);

					bx::prettify(tmp0, BX_COUNTOF(tmp0), currAvail);
					bx::prettify(tmp1, BX_COUNTOF(tmp1), totalAvail);
					tvm.printf(10, pos++, 0x8e, " Available: %10s / %10s ", tmp0, tmp1);

					bx::prettify(tmp0, BX_COUNTOF(tmp0), evictedCount);
					bx::prettify(tmp1, BX_COUNTOF(tmp1), evictedMemory);
					tvm.printf(10, pos++, 0x8e, "  Eviction: %10s / %10s ", tmp0, tmp1);
				}
#endif // BGFX_CONFIG_RENDERER_OPENGL

				pos++;
				double captureMs = double(captureElapsed)*toMs;
				tvm.printf(10, pos++, 0x8e, "    Capture: %7.4f [ms] ", captureMs);

				uint8_t attr[2] = { 0x89, 0x8a };
				uint8_t attrIndex = _render->m_waitSubmit < _render->m_waitRender;

				pos++;
				tvm.printf(10, pos++, attr[attrIndex&1], " Submit wait: %7.4f [ms] ", double(_render->m_waitSubmit)*toMs);
				tvm.printf(10, pos++, attr[(attrIndex+1)&1], " Render wait: %7.4f [ms] ", double(_render->m_waitRender)*toMs);

				min = frameTime;
				max = frameTime;
			}

			blit(this, _textVideoMemBlitter, tvm);
		}
		else if (_render->m_debug & BGFX_DEBUG_TEXT)
		{
			blit(this, _textVideoMemBlitter, _render->m_textVideoMem);
		}

		GL_CHECK(glFrameTerminatorGREMEDY() );
	}
} } // namespace bgfx

#else

namespace bgfx { namespace gl
{
	RendererContextI* rendererCreate()
	{
		return NULL;
	}

	void rendererDestroy()
	{
	}
} /* namespace gl */ } // namespace bgfx

#endif // (BGFX_CONFIG_RENDERER_OPENGLES || BGFX_CONFIG_RENDERER_OPENGL)

/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_NULL

namespace bgfx { namespace noop
{
	struct RendererContextNULL : public RendererContextI
	{
		RendererContextNULL()
		{
		}

		~RendererContextNULL()
		{
		}

		RendererType::Enum getRendererType() const BX_OVERRIDE
		{
			return RendererType::Null;
		}

		const char* getRendererName() const BX_OVERRIDE
		{
			return BGFX_RENDERER_NULL_NAME;
		}

		void flip(HMD& /*_hmd*/) BX_OVERRIDE
		{
		}

		void createIndexBuffer(IndexBufferHandle /*_handle*/, Memory* /*_mem*/, uint16_t /*_flags*/) BX_OVERRIDE
		{
		}

		void destroyIndexBuffer(IndexBufferHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createVertexDecl(VertexDeclHandle /*_handle*/, const VertexDecl& /*_decl*/) BX_OVERRIDE
		{
		}

		void destroyVertexDecl(VertexDeclHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createVertexBuffer(VertexBufferHandle /*_handle*/, Memory* /*_mem*/, VertexDeclHandle /*_declHandle*/, uint16_t /*_flags*/) BX_OVERRIDE
		{
		}

		void destroyVertexBuffer(VertexBufferHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createDynamicIndexBuffer(IndexBufferHandle /*_handle*/, uint32_t /*_size*/, uint16_t /*_flags*/) BX_OVERRIDE
		{
		}

		void updateDynamicIndexBuffer(IndexBufferHandle /*_handle*/, uint32_t /*_offset*/, uint32_t /*_size*/, Memory* /*_mem*/) BX_OVERRIDE
		{
		}

		void destroyDynamicIndexBuffer(IndexBufferHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createDynamicVertexBuffer(VertexBufferHandle /*_handle*/, uint32_t /*_size*/, uint16_t /*_flags*/) BX_OVERRIDE
		{
		}

		void updateDynamicVertexBuffer(VertexBufferHandle /*_handle*/, uint32_t /*_offset*/, uint32_t /*_size*/, Memory* /*_mem*/) BX_OVERRIDE
		{
		}

		void destroyDynamicVertexBuffer(VertexBufferHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createShader(ShaderHandle /*_handle*/, Memory* /*_mem*/) BX_OVERRIDE
		{
		}

		void destroyShader(ShaderHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createProgram(ProgramHandle /*_handle*/, ShaderHandle /*_vsh*/, ShaderHandle /*_fsh*/) BX_OVERRIDE
		{
		}

		void destroyProgram(ProgramHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createTexture(TextureHandle /*_handle*/, Memory* /*_mem*/, uint32_t /*_flags*/, uint8_t /*_skip*/) BX_OVERRIDE
		{
		}

		void updateTextureBegin(TextureHandle /*_handle*/, uint8_t /*_side*/, uint8_t /*_mip*/) BX_OVERRIDE
		{
		}

		void updateTexture(TextureHandle /*_handle*/, uint8_t /*_side*/, uint8_t /*_mip*/, const Rect& /*_rect*/, uint16_t /*_z*/, uint16_t /*_depth*/, uint16_t /*_pitch*/, const Memory* /*_mem*/) BX_OVERRIDE
		{
		}

		void updateTextureEnd() BX_OVERRIDE
		{
		}

		void resizeTexture(TextureHandle /*_handle*/, uint16_t /*_width*/, uint16_t /*_height*/) BX_OVERRIDE
		{
		}

		void destroyTexture(TextureHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createFrameBuffer(FrameBufferHandle /*_handle*/, uint8_t /*_num*/, const TextureHandle* /*_textureHandles*/) BX_OVERRIDE
		{
		}

		void createFrameBuffer(FrameBufferHandle /*_handle*/, void* /*_nwh*/, uint32_t /*_width*/, uint32_t /*_height*/, TextureFormat::Enum /*_depthFormat*/) BX_OVERRIDE
		{
		}

		void destroyFrameBuffer(FrameBufferHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void createUniform(UniformHandle /*_handle*/, UniformType::Enum /*_type*/, uint16_t /*_num*/, const char* /*_name*/) BX_OVERRIDE
		{
		}

		void destroyUniform(UniformHandle /*_handle*/) BX_OVERRIDE
		{
		}

		void saveScreenShot(const char* /*_filePath*/) BX_OVERRIDE
		{
		}

		void updateViewName(uint8_t /*_id*/, const char* /*_name*/) BX_OVERRIDE
		{
		}

		void updateUniform(uint16_t /*_loc*/, const void* /*_data*/, uint32_t /*_size*/) BX_OVERRIDE
		{
		}

		void setMarker(const char* /*_marker*/, uint32_t /*_size*/) BX_OVERRIDE
		{
		}

		void submit(Frame* /*_render*/, ClearQuad& /*_clearQuad*/, TextVideoMemBlitter& /*_textVideoMemBlitter*/) BX_OVERRIDE
		{
		}

		void blitSetup(TextVideoMemBlitter& /*_blitter*/) BX_OVERRIDE
		{
		}

		void blitRender(TextVideoMemBlitter& /*_blitter*/, uint32_t /*_numIndices*/) BX_OVERRIDE
		{
		}
	};

	static RendererContextNULL* s_renderNULL;

	RendererContextI* rendererCreate()
	{
		s_renderNULL = BX_NEW(g_allocator, RendererContextNULL);
		return s_renderNULL;
	}

	void rendererDestroy()
	{
		BX_DELETE(g_allocator, s_renderNULL);
		s_renderNULL = NULL;
	}
} /* namespace noop */ } // namespace bgfx

#else

namespace bgfx { namespace noop
{
	RendererContextI* rendererCreate()
	{
		return NULL;
	}

	void rendererDestroy()
	{
	}
} /* namespace noop */ } // namespace bgfx

#endif // BGFX_CONFIG_RENDERER_NULL
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"

#if BGFX_CONFIG_RENDERER_VULKAN
#	include "../../bgfx-ext/src/renderer_vk.cpp"
#else

namespace bgfx { namespace vk
{
	RendererContextI* rendererCreate()
	{
		return NULL;
	}

	void rendererDestroy()
	{
	}
} /* namespace vk */ } // namespace bgfx

#endif // BGFX_CONFIG_RENDERER_VULKAN
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"
#include "shader_dx9bc.h"

BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wunused-parameter");
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG("-Wunneeded-internal-declaration");

namespace bgfx
{
	struct Dx9bcOpcodeInfo
	{
		uint8_t numOperands;
		uint8_t numValues;
	};

	static const Dx9bcOpcodeInfo s_dx9bcOpcodeInfo[] =
	{
		{ 0, 0 }, // NOP
		{ 2, 0 }, // MOV
		{ 3, 0 }, // ADD
		{ 1, 0 }, // SUB
		{ 4, 0 }, // MAD
		{ 3, 0 }, // MUL
		{ 2, 0 }, // RCP
		{ 2, 0 }, // RSQ
		{ 3, 0 }, // DP3
		{ 3, 0 }, // DP4
		{ 3, 0 }, // MIN
		{ 3, 0 }, // MAX
		{ 3, 0 }, // SLT
		{ 3, 0 }, // SGE
		{ 2, 0 }, // EXP
		{ 2, 0 }, // LOG
		{ 1, 0 }, // LIT
		{ 1, 0 }, // DST
		{ 4, 0 }, // LRP
		{ 2, 0 }, // FRC
		{ 1, 0 }, // M4X4
		{ 1, 0 }, // M4X3
		{ 1, 0 }, // M3X4
		{ 1, 0 }, // M3X3
		{ 1, 0 }, // M3X2
		{ 0, 0 }, // CALL
		{ 0, 0 }, // CALLNZ
		{ 0, 0 }, // LOOP
		{ 0, 0 }, // RET
		{ 0, 0 }, // ENDLOOP
		{ 0, 0 }, // LABEL
		{ 1, 1 }, // DCL
		{ 3, 0 }, // POW
		{ 1, 0 }, // CRS
		{ 1, 0 }, // SGN
		{ 1, 0 }, // ABS
		{ 2, 0 }, // NRM
		{ 4, 0 }, // SINCOS
		{ 1, 0 }, // REP
		{ 0, 0 }, // ENDREP
		{ 1, 0 }, // IF
		{ 2, 0 }, // IFC
		{ 0, 0 }, // ELSE
		{ 0, 0 }, // ENDIF
		{ 0, 0 }, // BREAK
		{ 2, 0 }, // BREAKC
		{ 2, 0 }, // MOVA
		{ 1, 4 }, // DEFB
		{ 1, 4 }, // DEFI
		{ 0, 0 }, // 0
		{ 0, 0 }, // 1
		{ 0, 0 }, // 2
		{ 0, 0 }, // 3
		{ 0, 0 }, // 4
		{ 0, 0 }, // 5
		{ 0, 0 }, // 6
		{ 0, 0 }, // 7
		{ 0, 0 }, // 8
		{ 0, 0 }, // 9
		{ 0, 0 }, // 10
		{ 0, 0 }, // 11
		{ 0, 0 }, // 12
		{ 0, 0 }, // 13
		{ 0, 0 }, // 14
		{ 1, 0 }, // TEXCOORD
		{ 1, 0 }, // TEXKILL
		{ 3, 0 }, // TEX
		{ 1, 0 }, // TEXBEM
		{ 1, 0 }, // TEXBEM1
		{ 1, 0 }, // TEXREG2AR
		{ 1, 0 }, // TEXREG2GB
		{ 1, 0 }, // TEXM3X2PAD
		{ 1, 0 }, // TEXM3X2TEX
		{ 1, 0 }, // TEXM3X3PAD
		{ 1, 0 }, // TEXM3X3TEX
		{ 1, 0 }, // TEXM3X3DIFF
		{ 1, 0 }, // TEXM3X3SPEC
		{ 1, 0 }, // TEXM3X3VSPEC
		{ 2, 0 }, // EXPP
		{ 2, 0 }, // LOGP
		{ 4, 0 }, // CND
		{ 1, 4 }, // DEF
		{ 1, 0 }, // TEXREG2RGB
		{ 1, 0 }, // TEXDP3TEX
		{ 1, 0 }, // TEXM3X2DEPTH
		{ 1, 0 }, // TEXDP3
		{ 1, 0 }, // TEXM3X3
		{ 1, 0 }, // TEXDEPTH
		{ 4, 0 }, // CMP
		{ 1, 0 }, // BEM
		{ 4, 0 }, // DP2ADD
		{ 2, 0 }, // DSX
		{ 2, 0 }, // DSY
		{ 5, 0 }, // TEXLDD
		{ 1, 0 }, // SETP
		{ 3, 0 }, // TEXLDL
		{ 0, 0 }, // BREAKP
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dx9bcOpcodeInfo) == Dx9bcOpcode::Count);

	static const char* s_dx9bcOpcode[] =
	{
		"nop",
		"mov",
		"add",
		"sub",
		"mad",
		"mul",
		"rcp",
		"rsq",
		"dp3",
		"dp4",
		"min",
		"max",
		"slt",
		"sge",
		"exp",
		"log",
		"lit",
		"dst",
		"lrp",
		"frc",
		"m4x4",
		"m4x3",
		"m3x4",
		"m3x3",
		"m3x2",
		"call",
		"callnz",
		"loop",
		"ret",
		"endloop",
		"label",
		"dcl",
		"pow",
		"crs",
		"sgn",
		"abs",
		"nrm",
		"sincos",
		"rep",
		"endrep",
		"if",
		"ifc",
		"else",
		"endif",
		"break",
		"breakc",
		"mova",
		"defb",
		"defi",

		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,

		"texcoord",
		"texkill",
		"tex",
		"texbem",
		"texbem1",
		"texreg2ar",
		"texreg2gb",
		"texm3x2pad",
		"texm3x2tex",
		"texm3x3pad",
		"texm3x3tex",
		"texm3x3diff",
		"texm3x3spec",
		"texm3x3vspec",
		"expp",
		"logp",
		"cnd",
		"def",
		"texreg2rgb",
		"texdp3tex",
		"texm3x2depth",
		"texdp3",
		"texm3x3",
		"texdepth",
		"cmp",
		"bem",
		"dp2add",
		"dsx",
		"dsy",
		"texldd",
		"setp",
		"texldl",
		"breakp",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dx9bcOpcode) == Dx9bcOpcode::Count);

	const char* getName(Dx9bcOpcode::Enum _opcode)
	{
		BX_CHECK(_opcode < Dx9bcOpcode::Count, "Unknown opcode id %d (%x).", _opcode, _opcode);
		return s_dx9bcOpcode[_opcode];
	}

	static const char* s_dx9bcOperandType[] =
	{
		"r",           // Temporary Register File
		"v",           // Input Register File
		"c",           // Constant Register File
		"t",           // Texture Register File (PS)
		"oPos",        // Rasterizer Register File
		"oD",          // Attribute Output Register File
		"oT",          // Texture Coordinate Output Register File
		"output",      // Output register file for VS3.0+
		"i",           // Constant Integer Vector Register File
		"oColor",      // Color Output Register File
		"oDepth",      // Depth Output Register File
		"s",           // Sampler State Register File
		"c",           // Constant Register File  2048 - 4095
		"c",           // Constant Register File  4096 - 6143
		"c",           // Constant Register File  6144 - 8191
		"b",           // Constant Boolean register file
		"aL",          // Loop counter register file
		"tempfloat16", // 16-bit float temp register file
		"misctype",    // Miscellaneous (single) registers.
		"label",       // Label
		"p",           // Predicate register
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dx9bcOperandType) == Dx9bcOperandType::Count);

	static const char* s_dx9bcDeclUsage[] =
	{
		"position",
		"blendweight",
		"blendindices",
		"normal",
		"psize",
		"texcoord",
		"tangent",
		"binormal",
		"tessfactor",
		"positiont",
		"color",
		"fog",
		"depth",
		"sample",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dx9bcDeclUsage) == Dx9bcDeclUsage::Count);

	int32_t read(bx::ReaderI* _reader, Dx9bcSubOperand& _subOperand)
	{
		int32_t size = 0;

		uint32_t token;
		size += bx::read(_reader, token);

		_subOperand.type        =   Dx9bcOperandType::Enum( ( (token & UINT32_C(0x70000000) ) >> 28)
														  | ( (token & UINT32_C(0x00001800) ) >>  8) );
		_subOperand.regIndex    =                             (token & UINT32_C(0x000007ff) );
		_subOperand.swizzleBits =                      uint8_t( (token & UINT32_C(0x00ff0000) ) >> 16);

		return size;
	}

	int32_t write(bx::WriterI* _writer, const Dx9bcSubOperand& _subOperand)
	{
		int32_t size = 0;

		uint32_t token = 0;
		token |= (_subOperand.type        << 28) & UINT32_C(0x70000000);
		token |= (_subOperand.type        <<  8) & UINT32_C(0x00001800);
		token |=  _subOperand.regIndex           & UINT32_C(0x000007ff);
		token |= (_subOperand.swizzleBits << 16) & UINT32_C(0x00ff0000);
		size += bx::write(_writer, token);

		return size;
	}

	int32_t read(bx::ReaderI* _reader, Dx9bcOperand& _operand)
	{
		int32_t size = 0;

		uint32_t token;
		size += bx::read(_reader, token);

		_operand.type     =   Dx9bcOperandType::Enum( ( (token & UINT32_C(0x70000000) ) >> 28)
													| ( (token & UINT32_C(0x00001800) ) >>  8) );
		_operand.regIndex =                             (token & UINT32_C(0x000007ff) );
		_operand.addrMode = Dx9bcOperandAddrMode::Enum( (token & UINT32_C(0x00002000) ) >> 13);

		if (_operand.destination)
		{
			// Destination Parameter Token
			// https://msdn.microsoft.com/en-us/library/ff552738.aspx

			_operand.writeMask        = uint8_t( (token & UINT32_C(0x000f0000) ) >> 16);
			_operand.saturate         =     0 != (token & UINT32_C(0x00100000) );
			_operand.partialPrecision =     0 != (token & UINT32_C(0x00200000) );
			_operand.centroid         =     0 != (token & UINT32_C(0x00400000) );
		}
		else
		{
			// Source Parameter Token
			// https://msdn.microsoft.com/en-us/library/ff569716%28v=vs.85%29.aspx

			_operand.writeMask        = 0;
			_operand.saturate         = false;
			_operand.partialPrecision = false;
			_operand.centroid         = false;
			_operand.swizzleBits      = uint8_t( (token & UINT32_C(0x00ff0000) ) >> 16);
		}

		if (Dx9bcOperandAddrMode::Relative == _operand.addrMode)
		{
			size += read(_reader, _operand.subOperand);
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const Dx9bcOperand& _operand)
	{
		int32_t size = 0;

		uint32_t token = 0;
		token |= (_operand.type     << 28) & UINT32_C(0x70000000);
		token |= (_operand.type     <<  8) & UINT32_C(0x00001800);
		token |=  _operand.regIndex        & UINT32_C(0x000007ff);
		token |= (_operand.addrMode << 13) & UINT32_C(0x00002000);
		size += bx::write(_writer, token);

		if (Dx9bcOperandAddrMode::Relative == _operand.addrMode)
		{
			size += write(_writer, _operand.subOperand);
		}

		return size;
	}

	int32_t read(bx::ReaderI* _reader, Dx9bcInstruction& _instruction)
	{
		int32_t size = 0;

		uint32_t token;
		size += bx::read(_reader, token);

		_instruction.opcode = Dx9bcOpcode::Enum( (token & UINT32_C(0x0000ffff) ) );

		if (Dx9bcOpcode::Comment == _instruction.opcode)
		{
			_instruction.specific   = 0;
			_instruction.length     = uint16_t( (token & UINT32_C(0x7fff0000) ) >> 16) + 1;
			_instruction.predicated = false;
			_instruction.coissue    = false;
		}
		else
		{
			_instruction.specific   = uint8_t( (token & UINT32_C(0x00ff0000) ) >> 16);
			_instruction.length     = uint8_t( (token & UINT32_C(0x0f000000) ) >> 24) + 1;
			_instruction.predicated =     0 != (token & UINT32_C(0x10000000) );
			_instruction.coissue    =     0 != (token & UINT32_C(0x40000000) );
		}

		if (Dx9bcOpcode::Count <= _instruction.opcode)
		{
			if (Dx9bcOpcode::Comment == _instruction.opcode)
			{
				for (int32_t ii = 0, num = _instruction.length-1; ii < num; ++ii)
				{
					uint32_t tmp;
					size += bx::read(_reader, tmp);
				}
			}

			return size;
		}

		uint32_t currOp = 0;

		const Dx9bcOpcodeInfo& info = s_dx9bcOpcodeInfo[bx::uint32_min(_instruction.opcode, Dx9bcOpcode::Count)];
		_instruction.numOperands = info.numOperands;
		_instruction.numValues   = info.numValues;

		switch (_instruction.opcode)
		{
		case Dx9bcOpcode::SINCOS:
			if (5 > _instruction.length)
			{
				_instruction.numOperands = 2;
			}
			break;

		default:
			break;
		};

//BX_TRACE("%d (%d), %d, %d, 0x%08x"
//		, _instruction.opcode
//		, bx::uint32_min(_instruction.opcode, Dx9bcOpcode::Count)
//		, _instruction.length
//		, _instruction.numOperands
//		, token
//		);

		const bool valuesBeforeOpcode = false
				|| Dx9bcOpcode::DCL == _instruction.opcode
				;

		if (valuesBeforeOpcode
		&&  0 < info.numValues)
		{
			size += read(_reader, _instruction.value, info.numValues*sizeof(uint32_t) );
		}

		_instruction.operand[0].destination = true;

		switch (_instruction.numOperands)
		{
		case 6: size += read(_reader, _instruction.operand[currOp++]);
		case 5: size += read(_reader, _instruction.operand[currOp++]);
		case 4: size += read(_reader, _instruction.operand[currOp++]);
		case 3: size += read(_reader, _instruction.operand[currOp++]);
		case 2: size += read(_reader, _instruction.operand[currOp++]);
		case 1: size += read(_reader, _instruction.operand[currOp++]);
		case 0:
			if (!valuesBeforeOpcode
			&&  0 < info.numValues)
			{
				size += read(_reader, _instruction.value, info.numValues*sizeof(uint32_t) );
			}
			break;

		default:
			BX_CHECK(false, "Instruction %s with invalid number of operands %d (numValues %d)."
					, getName(_instruction.opcode)
					, _instruction.numOperands
					, info.numValues
					);
			break;
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const Dx9bcInstruction& _instruction)
	{
		int32_t size = 0;

		uint32_t token = 0;
		token |=    _instruction.opcode             & UINT32_C(0x0000ffff);
		token |=   (_instruction.specific    << 16) & UINT32_C(0x00ff0000);
		token |= ( (_instruction.length - 1) << 24) & UINT32_C(0x0f000000);
		size += bx::write(_writer, token);

		uint32_t currOp = 0;
		switch (_instruction.numOperands)
		{
		case 6: size += write(_writer, _instruction.operand[currOp++]);
		case 5: size += write(_writer, _instruction.operand[currOp++]);
		case 4: size += write(_writer, _instruction.operand[currOp++]);
		case 3: size += write(_writer, _instruction.operand[currOp++]);
		case 2: size += write(_writer, _instruction.operand[currOp++]);
		case 1: size += write(_writer, _instruction.operand[currOp++]);
		case 0:
			break;
		}

		return 0;
	}

	int32_t toString(char* _out, int32_t _size, const Dx9bcInstruction& _instruction)
	{
		int32_t size = 0;

		if (Dx9bcOpcode::Comment == _instruction.opcode
		||  Dx9bcOpcode::Phase   == _instruction.opcode)
		{
			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
						, "// %x"
						, _instruction.opcode
						);
			return size;
		}

		size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
							, "%2d %s"
							, _instruction.opcode
							, getName(_instruction.opcode)
							);

		switch (_instruction.opcode)
		{
		case Dx9bcOpcode::DCL:
			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
					, "_%s%d (%d, %d, %d, %d)"
					, s_dx9bcDeclUsage[_instruction.value[0] & UINT32_C(0x0000000f)]
					, (_instruction.value[0] & UINT32_C(0x000f0000) )>>16
					, (_instruction.value[0] & UINT32_C(0x08000000) )>>27 // ?
					, (_instruction.value[0] & UINT32_C(0x10000000) )>>28 // texture2d
					, (_instruction.value[0] & UINT32_C(0x20000000) )>>29 // textureCube
					, (_instruction.value[0] & UINT32_C(0x40000000) )>>30 // texture3d
					);
			break;

		default:
			break;
		}

		for (uint32_t ii = 0; ii < _instruction.numOperands; ++ii)
		{
			const Dx9bcOperand& operand = _instruction.operand[ii];
			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
								, "%s%s%d"
								, 0 == ii ? " " : ", "
								, s_dx9bcOperandType[operand.type]
								, operand.regIndex
								);

			if (operand.destination)
			{
				if (0xf > operand.writeMask
				&&  0   < operand.writeMask)
				{
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, ".%s%s%s%s"
										, 0 == (operand.writeMask & 1) ? "" : "x"
										, 0 == (operand.writeMask & 2) ? "" : "y"
										, 0 == (operand.writeMask & 4) ? "" : "z"
										, 0 == (operand.writeMask & 8) ? "" : "w"
										);
				}
			}
			else
			{
				if (Dx9bcOperandAddrMode::Relative == operand.addrMode)
				{
					const bool array = true;

					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "["
										);

					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "%s%d"
										, s_dx9bcOperandType[operand.subOperand.type]
										, operand.subOperand.regIndex
										);

					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "%s"
										, array ? "]" : ""
										);
				}

				if (0xe4 != operand.swizzleBits)
				{
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, ".%c%c%c%c"
										, "xyzw"[(operand.swizzleBits   )&0x3]
										, "xyzw"[(operand.swizzleBits>>2)&0x3]
										, "xyzw"[(operand.swizzleBits>>4)&0x3]
										, "xyzw"[(operand.swizzleBits>>6)&0x3]
										);
				}
			}
		}

		switch (_instruction.opcode)
		{
		case Dx9bcOpcode::DEF:
			for (uint32_t jj = 0; jj < _instruction.numValues; ++jj)
			{
				union { int32_t i; float f; } cast = { _instruction.value[jj] };
				size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
						, "%s%f%s"
						, 0 == jj ? " (" : ", "
						, cast.f
						, uint32_t(_instruction.numValues-1) == jj ? ")" : ""
						);
			}
			break;

		case Dx9bcOpcode::DEFI:
			for (uint32_t jj = 0; jj < _instruction.numValues; ++jj)
			{
				size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
						, "%s%d%s"
						, 0 == jj ? " (" : ", "
						, _instruction.value[jj]
						, uint32_t(_instruction.numValues-1) == jj ? ")" : ""
						);
			}
			break;

		default:
			break;
		}

		return size;
	}

	int32_t read(bx::ReaderSeekerI* _reader, Dx9bcShader& _shader)
	{
		int32_t size = 0;
		int64_t offset = bx::seek(_reader);

		for (;;)
		{
			Dx9bcInstruction instruction;
			int32_t length = read(_reader, instruction);
			size += length;

			if (Dx9bcOpcode::Count > instruction.opcode)
			{
				char temp[512];
				toString(temp, 512, instruction);

				BX_CHECK(length/4 == instruction.length
						, "%s\nread %d, expected %d"
						, temp
						, length/4
						, instruction.length
						);
			}
			else
			{
				if (Dx9bcOpcode::End == instruction.opcode)
				{
					size -= length;
					break;
				}
			}
		}

		bx::seek(_reader, offset, bx::Whence::Begin);

		_shader.byteCode.resize(size);
		bx::read(_reader, _shader.byteCode.data(), size);

		return size;
	}

	int32_t write(bx::WriterI* _writer, const Dx9bcShader& _shader)
	{
		BX_UNUSED(_writer, _shader);
		return 0;
	}

	int32_t read(bx::ReaderSeekerI* _reader, Dx9bc& _bc)
	{
		int32_t size = 0;

		size += bx::read(_reader, _bc.version);

		bool pixelShader = (0xffff0000 == (_bc.version & 0xffff0000) );
		uint32_t versionMajor = (_bc.version>>8)&0xff;
		uint32_t versionMinor = _bc.version&0xff;
		BX_UNUSED(pixelShader, versionMajor, versionMinor);
		BX_TRACE("%s shader %d.%d"
			, pixelShader ? "pixel" : "vertex"
			, versionMajor
			, versionMinor
			);

		size += read(_reader, _bc.shader);

		return size;
	}

	int32_t write(bx::WriterSeekerI* _writer, const Dx9bc& _dxbc)
	{
		BX_UNUSED(_writer, _dxbc);
		return 0;
	}

	void parse(const Dx9bcShader& _src, Dx9bcParseFn _fn, void* _userData)
	{
		bx::MemoryReader reader(_src.byteCode.data(), uint32_t(_src.byteCode.size() ) );

//BX_TRACE("parse %d", _src.byteCode.size());

		for (uint32_t token = 0, numTokens = uint32_t(_src.byteCode.size() / sizeof(uint32_t) ); token < numTokens;)
		{
			Dx9bcInstruction instruction;
			uint32_t size = read(&reader, instruction);
			BX_CHECK(size/4 == instruction.length, "read %d, expected %d", size/4, instruction.length); BX_UNUSED(size);

			_fn(token * sizeof(uint32_t), instruction, _userData);

			token += instruction.length;
		}
	}

	void filter(Dx9bcShader& _dst, const Dx9bcShader& _src, Dx9bcFilterFn _fn, void* _userData)
	{
		bx::MemoryReader reader(_src.byteCode.data(), uint32_t(_src.byteCode.size() ) );

		bx::CrtAllocator r;
		bx::MemoryBlock mb(&r);
		bx::MemoryWriter writer(&mb);

		for (uint32_t token = 0, numTokens = uint32_t(_src.byteCode.size() / sizeof(uint32_t) ); token < numTokens;)
		{
			Dx9bcInstruction instruction;
			uint32_t size = read(&reader, instruction);
			BX_CHECK(size/4 == instruction.length, "read %d, expected %d", size/4, instruction.length); BX_UNUSED(size);

			_fn(instruction, _userData);

			write(&writer, instruction);

			token += instruction.length;
		}

		uint8_t* data = (uint8_t*)mb.more();
		uint32_t size = uint32_t(bx::getSize(&writer) );
		_dst.byteCode.reserve(size);
		memcpy(_dst.byteCode.data(), data, size);
	}

} // namespace bgfx
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"
#include "shader_dxbc.h"

namespace bgfx
{
	struct DxbcOpcodeInfo
	{
		uint8_t numOperands;
		uint8_t numValues;
	};

	static const DxbcOpcodeInfo s_dxbcOpcodeInfo[] =
	{
		{ 3, 0 }, // ADD
		{ 3, 0 }, // AND
		{ 0, 0 }, // BREAK
		{ 1, 0 }, // BREAKC
		{ 0, 0 }, // CALL
		{ 0, 0 }, // CALLC
		{ 1, 0 }, // CASE
		{ 0, 0 }, // CONTINUE
		{ 1, 0 }, // CONTINUEC
		{ 0, 0 }, // CUT
		{ 0, 0 }, // DEFAULT
		{ 2, 0 }, // DERIV_RTX
		{ 2, 0 }, // DERIV_RTY
		{ 1, 0 }, // DISCARD
		{ 3, 0 }, // DIV
		{ 3, 0 }, // DP2
		{ 3, 0 }, // DP3
		{ 3, 0 }, // DP4
		{ 0, 0 }, // ELSE
		{ 0, 0 }, // EMIT
		{ 0, 0 }, // EMITTHENCUT
		{ 0, 0 }, // ENDIF
		{ 0, 0 }, // ENDLOOP
		{ 0, 0 }, // ENDSWITCH
		{ 3, 0 }, // EQ
		{ 2, 0 }, // EXP
		{ 2, 0 }, // FRC
		{ 2, 0 }, // FTOI
		{ 2, 0 }, // FTOU
		{ 3, 0 }, // GE
		{ 3, 0 }, // IADD
		{ 1, 0 }, // IF
		{ 3, 0 }, // IEQ
		{ 3, 0 }, // IGE
		{ 3, 0 }, // ILT
		{ 4, 0 }, // IMAD
		{ 3, 0 }, // IMAX
		{ 3, 0 }, // IMIN
		{ 4, 0 }, // IMUL
		{ 3, 0 }, // INE
		{ 2, 0 }, // INEG
		{ 3, 0 }, // ISHL
		{ 3, 0 }, // ISHR
		{ 2, 0 }, // ITOF
		{ 0, 0 }, // LABEL
		{ 3, 0 }, // LD
		{ 4, 0 }, // LD_MS
		{ 2, 0 }, // LOG
		{ 0, 0 }, // LOOP
		{ 3, 0 }, // LT
		{ 4, 0 }, // MAD
		{ 3, 0 }, // MIN
		{ 3, 0 }, // MAX
		{ 0, 1 }, // CUSTOMDATA
		{ 2, 0 }, // MOV
		{ 4, 0 }, // MOVC
		{ 3, 0 }, // MUL
		{ 3, 0 }, // NE
		{ 0, 0 }, // NOP
		{ 2, 0 }, // NOT
		{ 3, 0 }, // OR
		{ 3, 0 }, // RESINFO
		{ 0, 0 }, // RET
		{ 1, 0 }, // RETC
		{ 2, 0 }, // ROUND_NE
		{ 2, 0 }, // ROUND_NI
		{ 2, 0 }, // ROUND_PI
		{ 2, 0 }, // ROUND_Z
		{ 2, 0 }, // RSQ
		{ 4, 0 }, // SAMPLE
		{ 5, 0 }, // SAMPLE_C
		{ 5, 0 }, // SAMPLE_C_LZ
		{ 5, 0 }, // SAMPLE_L
		{ 6, 0 }, // SAMPLE_D
		{ 5, 0 }, // SAMPLE_B
		{ 2, 0 }, // SQRT
		{ 1, 0 }, // SWITCH
		{ 3, 0 }, // SINCOS
		{ 3, 0 }, // UDIV
		{ 3, 0 }, // ULT
		{ 3, 0 }, // UGE
		{ 4, 0 }, // UMUL
		{ 4, 0 }, // UMAD
		{ 3, 0 }, // UMAX
		{ 3, 0 }, // UMIN
		{ 3, 0 }, // USHR
		{ 2, 0 }, // UTOF
		{ 3, 0 }, // XOR
		{ 1, 1 }, // DCL_RESOURCE
		{ 1, 0 }, // DCL_CONSTANT_BUFFER
		{ 1, 0 }, // DCL_SAMPLER
		{ 1, 1 }, // DCL_INDEX_RANGE
		{ 1, 0 }, // DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY
		{ 1, 0 }, // DCL_GS_INPUT_PRIMITIVE
		{ 0, 1 }, // DCL_MAX_OUTPUT_VERTEX_COUNT
		{ 1, 0 }, // DCL_INPUT
		{ 1, 1 }, // DCL_INPUT_SGV
		{ 1, 0 }, // DCL_INPUT_SIV
		{ 1, 0 }, // DCL_INPUT_PS
		{ 1, 1 }, // DCL_INPUT_PS_SGV
		{ 1, 1 }, // DCL_INPUT_PS_SIV
		{ 1, 0 }, // DCL_OUTPUT
		{ 1, 0 }, // DCL_OUTPUT_SGV
		{ 1, 1 }, // DCL_OUTPUT_SIV
		{ 0, 1 }, // DCL_TEMPS
		{ 0, 3 }, // DCL_INDEXABLE_TEMP
		{ 0, 0 }, // DCL_GLOBAL_FLAGS

		{ 0, 0 }, // InstrD3D10
		{ 4, 0 }, // LOD
		{ 4, 0 }, // GATHER4
		{ 0, 0 }, // SAMPLE_POS
		{ 0, 0 }, // SAMPLE_INFO

		{ 0, 0 }, // InstrD3D10_1
		{ 0, 0 }, // HS_DECLS
		{ 0, 0 }, // HS_CONTROL_POINT_PHASE
		{ 0, 0 }, // HS_FORK_PHASE
		{ 0, 0 }, // HS_JOIN_PHASE
		{ 0, 0 }, // EMIT_STREAM
		{ 0, 0 }, // CUT_STREAM
		{ 1, 0 }, // EMITTHENCUT_STREAM
		{ 1, 0 }, // INTERFACE_CALL
		{ 0, 0 }, // BUFINFO
		{ 2, 0 }, // DERIV_RTX_COARSE
		{ 2, 0 }, // DERIV_RTX_FINE
		{ 2, 0 }, // DERIV_RTY_COARSE
		{ 2, 0 }, // DERIV_RTY_FINE
		{ 5, 0 }, // GATHER4_C
		{ 5, 0 }, // GATHER4_PO
		{ 0, 0 }, // GATHER4_PO_C
		{ 0, 0 }, // RCP
		{ 0, 0 }, // F32TOF16
		{ 0, 0 }, // F16TOF32
		{ 0, 0 }, // UADDC
		{ 0, 0 }, // USUBB
		{ 0, 0 }, // COUNTBITS
		{ 0, 0 }, // FIRSTBIT_HI
		{ 0, 0 }, // FIRSTBIT_LO
		{ 0, 0 }, // FIRSTBIT_SHI
		{ 0, 0 }, // UBFE
		{ 0, 0 }, // IBFE
		{ 5, 0 }, // BFI
		{ 0, 0 }, // BFREV
		{ 5, 0 }, // SWAPC
		{ 0, 0 }, // DCL_STREAM
		{ 1, 0 }, // DCL_FUNCTION_BODY
		{ 0, 0 }, // DCL_FUNCTION_TABLE
		{ 0, 0 }, // DCL_INTERFACE
		{ 0, 0 }, // DCL_INPUT_CONTROL_POINT_COUNT
		{ 0, 0 }, // DCL_OUTPUT_CONTROL_POINT_COUNT
		{ 0, 0 }, // DCL_TESS_DOMAIN
		{ 0, 0 }, // DCL_TESS_PARTITIONING
		{ 0, 0 }, // DCL_TESS_OUTPUT_PRIMITIVE
		{ 0, 0 }, // DCL_HS_MAX_TESSFACTOR
		{ 0, 0 }, // DCL_HS_FORK_PHASE_INSTANCE_COUNT
		{ 0, 0 }, // DCL_HS_JOIN_PHASE_INSTANCE_COUNT
		{ 0, 3 }, // DCL_THREAD_GROUP
		{ 1, 1 }, // DCL_UNORDERED_ACCESS_VIEW_TYPED
		{ 1, 0 }, // DCL_UNORDERED_ACCESS_VIEW_RAW
		{ 1, 1 }, // DCL_UNORDERED_ACCESS_VIEW_STRUCTURED
		{ 1, 1 }, // DCL_THREAD_GROUP_SHARED_MEMORY_RAW
		{ 1, 2 }, // DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED
		{ 1, 0 }, // DCL_RESOURCE_RAW
		{ 1, 1 }, // DCL_RESOURCE_STRUCTURED
		{ 3, 0 }, // LD_UAV_TYPED
		{ 3, 0 }, // STORE_UAV_TYPED
		{ 3, 0 }, // LD_RAW
		{ 3, 0 }, // STORE_RAW
		{ 4, 0 }, // LD_STRUCTURED
		{ 4, 0 }, // STORE_STRUCTURED
		{ 3, 0 }, // ATOMIC_AND
		{ 3, 0 }, // ATOMIC_OR
		{ 3, 0 }, // ATOMIC_XOR
		{ 3, 0 }, // ATOMIC_CMP_STORE
		{ 3, 0 }, // ATOMIC_IADD
		{ 3, 0 }, // ATOMIC_IMAX
		{ 3, 0 }, // ATOMIC_IMIN
		{ 3, 0 }, // ATOMIC_UMAX
		{ 3, 0 }, // ATOMIC_UMIN
		{ 2, 0 }, // IMM_ATOMIC_ALLOC
		{ 2, 0 }, // IMM_ATOMIC_CONSUME
		{ 0, 0 }, // IMM_ATOMIC_IADD
		{ 0, 0 }, // IMM_ATOMIC_AND
		{ 0, 0 }, // IMM_ATOMIC_OR
		{ 0, 0 }, // IMM_ATOMIC_XOR
		{ 0, 0 }, // IMM_ATOMIC_EXCH
		{ 0, 0 }, // IMM_ATOMIC_CMP_EXCH
		{ 0, 0 }, // IMM_ATOMIC_IMAX
		{ 0, 0 }, // IMM_ATOMIC_IMIN
		{ 0, 0 }, // IMM_ATOMIC_UMAX
		{ 0, 0 }, // IMM_ATOMIC_UMIN
		{ 0, 0 }, // SYNC
		{ 3, 0 }, // DADD
		{ 3, 0 }, // DMAX
		{ 3, 0 }, // DMIN
		{ 3, 0 }, // DMUL
		{ 3, 0 }, // DEQ
		{ 3, 0 }, // DGE
		{ 3, 0 }, // DLT
		{ 3, 0 }, // DNE
		{ 2, 0 }, // DMOV
		{ 4, 0 }, // DMOVC
		{ 0, 0 }, // DTOF
		{ 0, 0 }, // FTOD
		{ 3, 0 }, // EVAL_SNAPPED
		{ 3, 0 }, // EVAL_SAMPLE_INDEX
		{ 2, 0 }, // EVAL_CENTROID
		{ 0, 1 }, // DCL_GS_INSTANCE_COUNT
		{ 0, 0 }, // ABORT
		{ 0, 0 }, // DEBUG_BREAK

		{ 0, 0 }, // InstrD3D11
		{ 0, 0 }, // DDIV
		{ 0, 0 }, // DFMA
		{ 0, 0 }, // DRCP
		{ 0, 0 }, // MSAD
		{ 0, 0 }, // DTOI
		{ 0, 0 }, // DTOU
		{ 0, 0 }, // ITOD
		{ 0, 0 }, // UTOD
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dxbcOpcodeInfo) == DxbcOpcode::Count);

	static const char* s_dxbcOpcode[] =
	{
		"add",
		"and",
		"break",
		"breakc",
		"call",
		"callc",
		"case",
		"continue",
		"continuec",
		"cut",
		"default",
		"deriv_rtx",
		"deriv_rty",
		"discard",
		"div",
		"dp2",
		"dp3",
		"dp4",
		"else",
		"emit",
		"emitthencut",
		"endif",
		"endloop",
		"endswitch",
		"eq",
		"exp",
		"frc",
		"ftoi",
		"ftou",
		"ge",
		"iadd",
		"if",
		"ieq",
		"ige",
		"ilt",
		"imad",
		"imax",
		"imin",
		"imul",
		"ine",
		"ineg",
		"ishl",
		"ishr",
		"itof",
		"label",
		"ld",
		"ld_ms",
		"log",
		"loop",
		"lt",
		"mad",
		"min",
		"max",
		"customdata",
		"mov",
		"movc",
		"mul",
		"ne",
		"nop",
		"not",
		"or",
		"resinfo",
		"ret",
		"retc",
		"round_ne",
		"round_ni",
		"round_pi",
		"round_z",
		"rsq",
		"sample",
		"sample_c",
		"sample_c_lz",
		"sample_l",
		"sample_d",
		"sample_b",
		"sqrt",
		"switch",
		"sincos",
		"udiv",
		"ult",
		"uge",
		"umul",
		"umad",
		"umax",
		"umin",
		"ushr",
		"utof",
		"xor",
		"dcl_resource",
		"dcl_constantbuffer",
		"dcl_sampler",
		"dcl_index_range",
		"dcl_gs_output_primitive_topology",
		"dcl_gs_input_primitive",
		"dcl_max_output_vertex_count",
		"dcl_input",
		"dcl_input_sgv",
		"dcl_input_siv",
		"dcl_input_ps",
		"dcl_input_ps_sgv",
		"dcl_input_ps_siv",
		"dcl_output",
		"dcl_output_sgv",
		"dcl_output_siv",
		"dcl_temps",
		"dcl_indexable_temp",
		"dcl_global_flags",

		NULL,
		"lod",
		"gather4",
		"sample_pos",
		"sample_info",

		NULL,
		"hs_decls",
		"hs_control_point_phase",
		"hs_fork_phase",
		"hs_join_phase",
		"emit_stream",
		"cut_stream",
		"emitthencut_stream",
		"interface_call",
		"bufinfo",
		"deriv_rtx_coarse",
		"deriv_rtx_fine",
		"deriv_rty_coarse",
		"deriv_rty_fine",
		"gather4_c",
		"gather4_po",
		"gather4_po_c",
		"rcp",
		"f32tof16",
		"f16tof32",
		"uaddc",
		"usubb",
		"countbits",
		"firstbit_hi",
		"firstbit_lo",
		"firstbit_shi",
		"ubfe",
		"ibfe",
		"bfi",
		"bfrev",
		"swapc",
		"dcl_stream",
		"dcl_function_body",
		"dcl_function_table",
		"dcl_interface",
		"dcl_input_control_point_count",
		"dcl_output_control_point_count",
		"dcl_tess_domain",
		"dcl_tess_partitioning",
		"dcl_tess_output_primitive",
		"dcl_hs_max_tessfactor",
		"dcl_hs_fork_phase_instance_count",
		"dcl_hs_join_phase_instance_count",
		"dcl_thread_group",
		"dcl_unordered_access_view_typed",
		"dcl_unordered_access_view_raw",
		"dcl_unordered_access_view_structured",
		"dcl_thread_group_shared_memory_raw",
		"dcl_thread_group_shared_memory_structured",
		"dcl_resource_raw",
		"dcl_resource_structured",
		"ld_uav_typed",
		"store_uav_typed",
		"ld_raw",
		"store_raw",
		"ld_structured",
		"store_structured",
		"atomic_and",
		"atomic_or",
		"atomic_xor",
		"atomic_cmp_store",
		"atomic_iadd",
		"atomic_imax",
		"atomic_imin",
		"atomic_umax",
		"atomic_umin",
		"imm_atomic_alloc",
		"imm_atomic_consume",
		"imm_atomic_iadd",
		"imm_atomic_and",
		"imm_atomic_or",
		"imm_atomic_xor",
		"imm_atomic_exch",
		"imm_atomic_cmp_exch",
		"imm_atomic_imax",
		"imm_atomic_imin",
		"imm_atomic_umax",
		"imm_atomic_umin",
		"sync",
		"dadd",
		"dmax",
		"dmin",
		"dmul",
		"deq",
		"dge",
		"dlt",
		"dne",
		"dmov",
		"dmovc",
		"dtof",
		"ftod",
		"eval_snapped",
		"eval_sample_index",
		"eval_centroid",
		"dcl_gs_instance_count",
		"abort",
		"debug_break",

		NULL,
		"ddiv",
		"dfma",
		"drcp",
		"msad",
		"dtoi",
		"dtou",
		"itod",
		"utod",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dxbcOpcode) == DxbcOpcode::Count);

	const char* getName(DxbcOpcode::Enum _opcode)
	{
		BX_CHECK(_opcode < DxbcOpcode::Count, "Unknown opcode id %d.", _opcode);
		return s_dxbcOpcode[_opcode];
	}

	static const char* s_dxbcSrvType[] =
	{
		"",                 // Unknown
		"Buffer",           // Buffer
		"Texture1D",        // Texture1D
		"Texture2D",        // Texture2D
		"Texture2DMS",      // Texture2DMS
		"Texture3D",        // Texture3D
		"TextureCube",      // TextureCube
		"Texture1DArray",   // Texture1DArray
		"Texture2DArray",   // Texture2DArray
		"Texture2DMSArray", // Texture2DMSArray
		"TextureCubearray", // TextureCubearray
		"RawBuffer",        // RawBuffer
		"StructuredBuffer", // StructuredBuffer
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dxbcSrvType) == DxbcResourceDim::Count);

	static const char* s_dxbcInterpolationName[] =
	{
		"",
		"constant",
		"linear",
		"linear centroid",
		"linear noperspective",
		"linear noperspective centroid",
		"linear sample",
		"linear noperspective sample",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dxbcInterpolationName) == DxbcInterpolation::Count);

	// mesa/src/gallium/state_trackers/d3d1x/d3d1xshader/defs/shortfiles.txt
	static const char* s_dxbcOperandType[] =
	{
		"r",                         // Temp
		"v",                         // Input
		"o",                         // Output
		"x",                         // TempArray
		"l",                         // Imm32
		"d",                         // Imm64
		"s",                         // Sampler
		"t",                         // Resource
		"cb",                        // ConstantBuffer
		"icb",                       // ImmConstantBuffer
		"label",                     // Label
		"vPrim",                     // PrimitiveID
		"oDepth",                    // OutputDepth
		"null",                      // Null
		"rasterizer",                // Rasterizer
		"oMask",                     // CoverageMask
		"stream",                    // Stream
		"function_body",             // FunctionBody
		"function_table",            // FunctionTable
		"interface",                 // Interface
		"function_input",            // FunctionInput
		"function_output",           // FunctionOutput
		"vOutputControlPointID",     // OutputControlPointId
		"vForkInstanceID",           // InputForkInstanceId
		"vJoinInstanceID",           // InputJoinInstanceId
		"vicp",                      // InputControlPoint
		"vocp",                      // OutputControlPoint
		"vpc",                       // InputPatchConstant
		"vDomain",                   // InputDomainPoint
		"this",                      // ThisPointer
		"u",                         // UnorderedAccessView
		"g",                         // ThreadGroupSharedMemory
		"vThreadID",                 // InputThreadId
		"vThreadGrouID",             // InputThreadGroupId
		"vThreadIDInGroup",          // InputThreadIdInGroup
		"vCoverage",                 // InputCoverageMask
		"vThreadIDInGroupFlattened", // InputThreadIdInGroupFlattened
		"vGSInstanceID",             // InputGsInstanceId
		"oDepthGE",                  // OutputDepthGreaterEqual
		"oDepthLE",                  // OutputDepthLessEqual
		"vCycleCounter",             // CycleCounter
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_dxbcOperandType) == DxbcOperandType::Count);

#define DXBC_MAX_NAME_STRING 512

	int32_t readString(bx::ReaderSeekerI* _reader, int64_t _offset, char* _out, uint32_t _max = DXBC_MAX_NAME_STRING)
	{
		int64_t oldOffset = bx::seek(_reader);
		bx::seek(_reader, _offset, bx::Whence::Begin);

		int32_t size = 0;

		for (uint32_t ii = 0; ii < _max-1; ++ii)
		{
			char ch;
			size += bx::read(_reader, ch);
			*_out++ = ch;

			if ('\0' == ch)
			{
				break;
			}
		}
		*_out = '\0';

		bx::seek(_reader, oldOffset, bx::Whence::Begin);

		return size;
	}

	inline uint32_t dxbcMixF(uint32_t _b, uint32_t _c, uint32_t _d)
	{
		const uint32_t tmp0   = bx::uint32_xor(_c, _d);
		const uint32_t tmp1   = bx::uint32_and(_b, tmp0);
		const uint32_t result = bx::uint32_xor(_d, tmp1);

		return result;
	}

	inline uint32_t dxbcMixG(uint32_t _b, uint32_t _c, uint32_t _d)
	{
		return dxbcMixF(_d, _b, _c);
	}

	inline uint32_t dxbcMixH(uint32_t _b, uint32_t _c, uint32_t _d)
	{
		const uint32_t tmp0   = bx::uint32_xor(_b, _c);
		const uint32_t result = bx::uint32_xor(_d, tmp0);

		return result;
	}

	inline uint32_t dxbcMixI(uint32_t _b, uint32_t _c, uint32_t _d)
	{
		const uint32_t tmp0   = bx::uint32_orc(_b, _d);
		const uint32_t result = bx::uint32_xor(_c, tmp0);

		return result;
	}

	void dxbcHashBlock(const uint32_t* data, uint32_t* hash)
	{
		const uint32_t d0  = data[ 0];
		const uint32_t d1  = data[ 1];
		const uint32_t d2  = data[ 2];
		const uint32_t d3  = data[ 3];
		const uint32_t d4  = data[ 4];
		const uint32_t d5  = data[ 5];
		const uint32_t d6  = data[ 6];
		const uint32_t d7  = data[ 7];
		const uint32_t d8  = data[ 8];
		const uint32_t d9  = data[ 9];
		const uint32_t d10 = data[10];
		const uint32_t d11 = data[11];
		const uint32_t d12 = data[12];
		const uint32_t d13 = data[13];
		const uint32_t d14 = data[14];
		const uint32_t d15 = data[15];

		uint32_t aa = hash[0];
		uint32_t bb = hash[1];
		uint32_t cc = hash[2];
		uint32_t dd = hash[3];

		aa = bb + bx::uint32_rol(aa + dxbcMixF(bb, cc, dd) + d0  + 0xd76aa478,  7);
		dd = aa + bx::uint32_rol(dd + dxbcMixF(aa, bb, cc) + d1  + 0xe8c7b756, 12);
		cc = dd + bx::uint32_ror(cc + dxbcMixF(dd, aa, bb) + d2  + 0x242070db, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixF(cc, dd, aa) + d3  + 0xc1bdceee, 10);
		aa = bb + bx::uint32_rol(aa + dxbcMixF(bb, cc, dd) + d4  + 0xf57c0faf,  7);
		dd = aa + bx::uint32_rol(dd + dxbcMixF(aa, bb, cc) + d5  + 0x4787c62a, 12);
		cc = dd + bx::uint32_ror(cc + dxbcMixF(dd, aa, bb) + d6  + 0xa8304613, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixF(cc, dd, aa) + d7  + 0xfd469501, 10);
		aa = bb + bx::uint32_rol(aa + dxbcMixF(bb, cc, dd) + d8  + 0x698098d8,  7);
		dd = aa + bx::uint32_rol(dd + dxbcMixF(aa, bb, cc) + d9  + 0x8b44f7af, 12);
		cc = dd + bx::uint32_ror(cc + dxbcMixF(dd, aa, bb) + d10 + 0xffff5bb1, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixF(cc, dd, aa) + d11 + 0x895cd7be, 10);
		aa = bb + bx::uint32_rol(aa + dxbcMixF(bb, cc, dd) + d12 + 0x6b901122,  7);
		dd = aa + bx::uint32_rol(dd + dxbcMixF(aa, bb, cc) + d13 + 0xfd987193, 12);
		cc = dd + bx::uint32_ror(cc + dxbcMixF(dd, aa, bb) + d14 + 0xa679438e, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixF(cc, dd, aa) + d15 + 0x49b40821, 10);

		aa = bb + bx::uint32_rol(aa + dxbcMixG(bb, cc, dd) + d1  + 0xf61e2562,  5);
		dd = aa + bx::uint32_rol(dd + dxbcMixG(aa, bb, cc) + d6  + 0xc040b340,  9);
		cc = dd + bx::uint32_rol(cc + dxbcMixG(dd, aa, bb) + d11 + 0x265e5a51, 14);
		bb = cc + bx::uint32_ror(bb + dxbcMixG(cc, dd, aa) + d0  + 0xe9b6c7aa, 12);
		aa = bb + bx::uint32_rol(aa + dxbcMixG(bb, cc, dd) + d5  + 0xd62f105d,  5);
		dd = aa + bx::uint32_rol(dd + dxbcMixG(aa, bb, cc) + d10 + 0x02441453,  9);
		cc = dd + bx::uint32_rol(cc + dxbcMixG(dd, aa, bb) + d15 + 0xd8a1e681, 14);
		bb = cc + bx::uint32_ror(bb + dxbcMixG(cc, dd, aa) + d4  + 0xe7d3fbc8, 12);
		aa = bb + bx::uint32_rol(aa + dxbcMixG(bb, cc, dd) + d9  + 0x21e1cde6,  5);
		dd = aa + bx::uint32_rol(dd + dxbcMixG(aa, bb, cc) + d14 + 0xc33707d6,  9);
		cc = dd + bx::uint32_rol(cc + dxbcMixG(dd, aa, bb) + d3  + 0xf4d50d87, 14);
		bb = cc + bx::uint32_ror(bb + dxbcMixG(cc, dd, aa) + d8  + 0x455a14ed, 12);
		aa = bb + bx::uint32_rol(aa + dxbcMixG(bb, cc, dd) + d13 + 0xa9e3e905,  5);
		dd = aa + bx::uint32_rol(dd + dxbcMixG(aa, bb, cc) + d2  + 0xfcefa3f8,  9);
		cc = dd + bx::uint32_rol(cc + dxbcMixG(dd, aa, bb) + d7  + 0x676f02d9, 14);
		bb = cc + bx::uint32_ror(bb + dxbcMixG(cc, dd, aa) + d12 + 0x8d2a4c8a, 12);

		aa = bb + bx::uint32_rol(aa + dxbcMixH(bb, cc, dd) + d5  + 0xfffa3942,  4);
		dd = aa + bx::uint32_rol(dd + dxbcMixH(aa, bb, cc) + d8  + 0x8771f681, 11);
		cc = dd + bx::uint32_rol(cc + dxbcMixH(dd, aa, bb) + d11 + 0x6d9d6122, 16);
		bb = cc + bx::uint32_ror(bb + dxbcMixH(cc, dd, aa) + d14 + 0xfde5380c,  9);
		aa = bb + bx::uint32_rol(aa + dxbcMixH(bb, cc, dd) + d1  + 0xa4beea44,  4);
		dd = aa + bx::uint32_rol(dd + dxbcMixH(aa, bb, cc) + d4  + 0x4bdecfa9, 11);
		cc = dd + bx::uint32_rol(cc + dxbcMixH(dd, aa, bb) + d7  + 0xf6bb4b60, 16);
		bb = cc + bx::uint32_ror(bb + dxbcMixH(cc, dd, aa) + d10 + 0xbebfbc70,  9);
		aa = bb + bx::uint32_rol(aa + dxbcMixH(bb, cc, dd) + d13 + 0x289b7ec6,  4);
		dd = aa + bx::uint32_rol(dd + dxbcMixH(aa, bb, cc) + d0  + 0xeaa127fa, 11);
		cc = dd + bx::uint32_rol(cc + dxbcMixH(dd, aa, bb) + d3  + 0xd4ef3085, 16);
		bb = cc + bx::uint32_ror(bb + dxbcMixH(cc, dd, aa) + d6  + 0x04881d05,  9);
		aa = bb + bx::uint32_rol(aa + dxbcMixH(bb, cc, dd) + d9  + 0xd9d4d039,  4);
		dd = aa + bx::uint32_rol(dd + dxbcMixH(aa, bb, cc) + d12 + 0xe6db99e5, 11);
		cc = dd + bx::uint32_rol(cc + dxbcMixH(dd, aa, bb) + d15 + 0x1fa27cf8, 16);
		bb = cc + bx::uint32_ror(bb + dxbcMixH(cc, dd, aa) + d2  + 0xc4ac5665,  9);

		aa = bb + bx::uint32_rol(aa + dxbcMixI(bb, cc, dd) + d0  + 0xf4292244,  6);
		dd = aa + bx::uint32_rol(dd + dxbcMixI(aa, bb, cc) + d7  + 0x432aff97, 10);
		cc = dd + bx::uint32_rol(cc + dxbcMixI(dd, aa, bb) + d14 + 0xab9423a7, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixI(cc, dd, aa) + d5  + 0xfc93a039, 11);
		aa = bb + bx::uint32_rol(aa + dxbcMixI(bb, cc, dd) + d12 + 0x655b59c3,  6);
		dd = aa + bx::uint32_rol(dd + dxbcMixI(aa, bb, cc) + d3  + 0x8f0ccc92, 10);
		cc = dd + bx::uint32_rol(cc + dxbcMixI(dd, aa, bb) + d10 + 0xffeff47d, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixI(cc, dd, aa) + d1  + 0x85845dd1, 11);
		aa = bb + bx::uint32_rol(aa + dxbcMixI(bb, cc, dd) + d8  + 0x6fa87e4f,  6);
		dd = aa + bx::uint32_rol(dd + dxbcMixI(aa, bb, cc) + d15 + 0xfe2ce6e0, 10);
		cc = dd + bx::uint32_rol(cc + dxbcMixI(dd, aa, bb) + d6  + 0xa3014314, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixI(cc, dd, aa) + d13 + 0x4e0811a1, 11);
		aa = bb + bx::uint32_rol(aa + dxbcMixI(bb, cc, dd) + d4  + 0xf7537e82,  6);
		dd = aa + bx::uint32_rol(dd + dxbcMixI(aa, bb, cc) + d11 + 0xbd3af235, 10);
		cc = dd + bx::uint32_rol(cc + dxbcMixI(dd, aa, bb) + d2  + 0x2ad7d2bb, 15);
		bb = cc + bx::uint32_ror(bb + dxbcMixI(cc, dd, aa) + d9  + 0xeb86d391, 11);

		hash[0] += aa;
		hash[1] += bb;
		hash[2] += cc;
		hash[3] += dd;
	}

	// dxbc hash function is slightly modified version of MD5 hash.
	// https://tools.ietf.org/html/rfc1321
	// http://www.efgh.com/software/md5.txt
	//
	// Assumption is that data pointer, size are both 4-byte aligned,
	// and little endian.
	//
	void dxbcHash(const void* _data, uint32_t _size, void* _digest)
	{
		uint32_t hash[4] =
		{
			0x67452301,
			0xefcdab89,
			0x98badcfe,
			0x10325476,
		};

		const uint32_t* data = (const uint32_t*)_data;
		for (uint32_t ii = 0, num = _size/64; ii < num; ++ii)
		{
			dxbcHashBlock(data, hash);
			data += 16;
		}

		uint32_t last[16];
		memset(last, 0, sizeof(last) );

		const uint32_t remaining = _size & 0x3f;

		if (remaining >= 56)
		{
			memcpy(&last[0], data, remaining);
			last[remaining/4] = 0x80;
			dxbcHashBlock(last, hash);

			memset(&last[1], 0, 56);
		}
		else
		{
			memcpy(&last[1], data, remaining);
			last[1 + remaining/4] = 0x80;
		}

		last[ 0] = _size * 8;
		last[15] = _size * 2 + 1;
		dxbcHashBlock(last, hash);

		memcpy(_digest, hash, 16);
	}

	int32_t read(bx::ReaderI* _reader, DxbcSubOperand& _subOperand)
	{
		uint32_t token;
		int32_t size = 0;

		// 0       1       2       3
		// 76543210765432107654321076543210
		// e222111000nnttttttttssssssssmmoo
		// ^^  ^  ^  ^ ^       ^       ^ ^-- number of operands
		// ||  |  |  | |       |       +---- operand mode
		// ||  |  |  | |       +------------ operand mode bits
		// ||  |  |  | +-------------------- type
		// ||  |  |  +---------------------- number of addressing modes
		// ||  |  +------------------------- addressing mode 0
		// ||  +---------------------------- addressing mode 1
		// |+------------------------------- addressing mode 2
		// +-------------------------------- extended

		size += bx::read(_reader, token);
		_subOperand.type         = DxbcOperandType::Enum( (token & UINT32_C(0x000ff000) ) >> 12);
		_subOperand.numAddrModes =               uint8_t( (token & UINT32_C(0x00300000) ) >> 20);
		_subOperand.addrMode     =               uint8_t( (token & UINT32_C(0x01c00000) ) >> 22);
		_subOperand.mode         = DxbcOperandMode::Enum( (token & UINT32_C(0x0000000c) ) >>  2);
		_subOperand.modeBits     =               uint8_t( (token & UINT32_C(0x00000ff0) ) >>  4) & "\x0f\xff\x03\x00"[_subOperand.mode];
		_subOperand.num          =               uint8_t( (token & UINT32_C(0x00000003) )      );

		switch (_subOperand.addrMode)
		{
		case DxbcOperandAddrMode::Imm32:
			size += bx::read(_reader, _subOperand.regIndex);
			break;

		case DxbcOperandAddrMode::Reg:
			{
				DxbcSubOperand subOperand;
				size += read(_reader, subOperand);
			}
			break;

		case DxbcOperandAddrMode::RegImm32:
			{
				size += bx::read(_reader, _subOperand.regIndex);

				DxbcSubOperand subOperand;
				size += read(_reader, subOperand);
			}
			break;

		case DxbcOperandAddrMode::RegImm64:
			{
				size += bx::read(_reader, _subOperand.regIndex);
				size += bx::read(_reader, _subOperand.regIndex);

				DxbcSubOperand subOperand;
				size += read(_reader, subOperand);
			}
			break;

		default:
			BX_CHECK(false, "sub operand addressing mode %d", _subOperand.addrMode);
			break;
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const DxbcSubOperand& _subOperand)
	{
		int32_t size = 0;

		uint32_t token = 0;
		token |= (_subOperand.type         << 12) & UINT32_C(0x000ff000);
		token |= (_subOperand.numAddrModes << 20) & UINT32_C(0x00300000);
		token |= (_subOperand.addrMode     << 22) & UINT32_C(0x01c00000);
		token |= (_subOperand.mode         <<  2) & UINT32_C(0x0000000c);
		token |= (_subOperand.modeBits     <<  4) & UINT32_C(0x00000ff0);
		token |=  _subOperand.num                 & UINT32_C(0x00000003);
		size += bx::write(_writer, token);

		switch (_subOperand.addrMode)
		{
		case DxbcOperandAddrMode::Imm32:
			size += bx::write(_writer, _subOperand.regIndex);
			break;

		case DxbcOperandAddrMode::Reg:
			{
				DxbcSubOperand subOperand;
				size += write(_writer, subOperand);
			}
			break;

		case DxbcOperandAddrMode::RegImm32:
			{
				size += bx::write(_writer, _subOperand.regIndex);

				DxbcSubOperand subOperand;
				size += write(_writer, subOperand);
			}
			break;

		case DxbcOperandAddrMode::RegImm64:
			{
				size += bx::write(_writer, _subOperand.regIndex);
				size += bx::write(_writer, _subOperand.regIndex);

				DxbcSubOperand subOperand;
				size += write(_writer, subOperand);
			}
			break;

		default:
			BX_CHECK(false, "sub operand addressing mode %d", _subOperand.addrMode);
			break;
		}

		return size;
	}

	int32_t read(bx::ReaderI* _reader, DxbcOperand& _operand)
	{
		int32_t size = 0;

		uint32_t token;
		size += bx::read(_reader, token);

		// 0       1       2       3
		// 76543210765432107654321076543210
		// e222111000nnttttttttssssssssmmoo
		// ^^  ^  ^  ^ ^       ^       ^ ^-- number of operands
		// ||  |  |  | |       |       +---- operand mode
		// ||  |  |  | |       +------------ operand mode bits
		// ||  |  |  | +-------------------- type
		// ||  |  |  +---------------------- number of addressing modes
		// ||  |  +------------------------- addressing mode 0
		// ||  +---------------------------- addressing mode 1
		// |+------------------------------- addressing mode 2
		// +-------------------------------- extended

		_operand.extended     =                   0 != (token & UINT32_C(0x80000000) );
		_operand.numAddrModes =               uint8_t( (token & UINT32_C(0x00300000) ) >> 20);
		_operand.addrMode[0]  =               uint8_t( (token & UINT32_C(0x01c00000) ) >> 22);
		_operand.addrMode[1]  =               uint8_t( (token & UINT32_C(0x0e000000) ) >> 25);
		_operand.addrMode[2]  =               uint8_t( (token & UINT32_C(0x70000000) ) >> 28);
		_operand.type         = DxbcOperandType::Enum( (token & UINT32_C(0x000ff000) ) >> 12);
		_operand.mode         = DxbcOperandMode::Enum( (token & UINT32_C(0x0000000c) ) >>  2);
		_operand.modeBits     =               uint8_t( (token & UINT32_C(0x00000ff0) ) >>  4) & "\x0f\xff\x03\x00"[_operand.mode];
		_operand.num          =               uint8_t( (token & UINT32_C(0x00000003) )      );

		if (_operand.extended)
		{
			size += bx::read(_reader, _operand.extBits);
		}

		switch (_operand.type)
		{
		case DxbcOperandType::Imm32:
			_operand.num = 2 == _operand.num ? 4 : _operand.num;
			for (uint32_t ii = 0; ii < _operand.num; ++ii)
			{
				size += bx::read(_reader, _operand.un.imm32[ii]);
			}
			break;

		case DxbcOperandType::Imm64:
			_operand.num = 2 == _operand.num ? 4 : _operand.num;
			for (uint32_t ii = 0; ii < _operand.num; ++ii)
			{
				size += bx::read(_reader, _operand.un.imm64[ii]);
			}
			break;

		default:
			break;
		}

		for (uint32_t ii = 0; ii < _operand.numAddrModes; ++ii)
		{
			switch (_operand.addrMode[ii])
			{
			case DxbcOperandAddrMode::Imm32:
				size += bx::read(_reader, _operand.regIndex[ii]);
				break;

			case DxbcOperandAddrMode::Reg:
				size += read(_reader, _operand.subOperand[ii]);
				break;

			case DxbcOperandAddrMode::RegImm32:
				size += bx::read(_reader, _operand.regIndex[ii]);
				size += read(_reader, _operand.subOperand[ii]);
				break;

			default:
				BX_CHECK(false, "operand %d addressing mode %d", ii, _operand.addrMode[ii]);
				break;
			}
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const DxbcOperand& _operand)
	{
		int32_t size = 0;

		uint32_t token = 0;
		token |=  _operand.extended            ? UINT32_C(0x80000000) : 0;
		token |= (_operand.numAddrModes << 20) & UINT32_C(0x00300000);
		token |= (_operand.addrMode[0]  << 22) & UINT32_C(0x01c00000);
		token |= (_operand.addrMode[1]  << 25) & UINT32_C(0x0e000000);
		token |= (_operand.addrMode[2]  << 28) & UINT32_C(0x70000000);
		token |= (_operand.type         << 12) & UINT32_C(0x000ff000);
		token |= (_operand.mode         <<  2) & UINT32_C(0x0000000c);

		token |= (4 == _operand.num ? 2 : _operand.num) & UINT32_C(0x00000003);
		token |= ( (_operand.modeBits & "\x0f\xff\x03\x00"[_operand.mode]) << 4) & UINT32_C(0x00000ff0);

		size += bx::write(_writer, token);

		if (_operand.extended)
		{
			size += bx::write(_writer, _operand.extBits);
		}

		switch (_operand.type)
		{
		case DxbcOperandType::Imm32:
			for (uint32_t ii = 0; ii < _operand.num; ++ii)
			{
				size += bx::write(_writer, _operand.un.imm32[ii]);
			}
			break;

		case DxbcOperandType::Imm64:
			for (uint32_t ii = 0; ii < _operand.num; ++ii)
			{
				size += bx::write(_writer, _operand.un.imm64[ii]);
			}
			break;

		default:
			break;
		}

		for (uint32_t ii = 0; ii < _operand.numAddrModes; ++ii)
		{
			switch (_operand.addrMode[ii])
			{
			case DxbcOperandAddrMode::Imm32:
				size += bx::write(_writer, _operand.regIndex[ii]);
				break;

			case DxbcOperandAddrMode::Reg:
				size += write(_writer, _operand.subOperand[ii]);
				break;

			case DxbcOperandAddrMode::RegImm32:
				size += bx::write(_writer, _operand.regIndex[ii]);
				size += write(_writer, _operand.subOperand[ii]);
				break;

			default:
				BX_CHECK(false, "operand %d addressing mode %d", ii, _operand.addrMode[ii]);
				break;
			}
		}

		return size;
	}

	int32_t read(bx::ReaderI* _reader, DxbcInstruction& _instruction)
	{
		uint32_t size = 0;

		uint32_t token;
		size += bx::read(_reader, token);

		// 0       1       2       3
		// 76543210765432107654321076543210
		// elllllll.............ooooooooooo
		// ^^                   ^----------- opcode
		// |+------------------------------- length
		// +-------------------------------- extended

		_instruction.opcode = DxbcOpcode::Enum( (token & UINT32_C(0x000007ff) )      );
		_instruction.length =          uint8_t( (token & UINT32_C(0x7f000000) ) >> 24);
		bool extended       =              0 != (token & UINT32_C(0x80000000) );

		_instruction.srv     = DxbcResourceDim::Unknown;
		_instruction.samples = 0;

		_instruction.shadow = false;
		_instruction.mono   = false;

		_instruction.allowRefactoring = false;
		_instruction.fp64             = false;
		_instruction.earlyDepth       = false;
		_instruction.enableBuffers    = false;
		_instruction.skipOptimization = false;
		_instruction.enableMinPrecision     = false;
		_instruction.enableDoubleExtensions = false;
		_instruction.enableShaderExtensions = false;

		_instruction.threadsInGroup = false;
		_instruction.sharedMemory   = false;
		_instruction.uavGroup       = false;
		_instruction.uavGlobal      = false;

		_instruction.saturate = false;
		_instruction.testNZ   = false;
		_instruction.retType  = DxbcResourceReturnType::Unused;

		switch (_instruction.opcode)
		{
			case DxbcOpcode::CUSTOMDATA:
				{
//					uint32_t dataClass;
					size += bx::read(_reader, _instruction.length);
					for (uint32_t ii = 0, num = (_instruction.length-2)/4; ii < num; ++ii)
					{
						char temp[16];
						size += bx::read(_reader, temp, 16);
					}

				}
				return size;

			case DxbcOpcode::DCL_CONSTANT_BUFFER:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........            a...........
				//                     ^------------ Allow refactoring

				_instruction.allowRefactoring = 0 != (token & UINT32_C(0x00000800) );
				break;

			case DxbcOpcode::DCL_GLOBAL_FLAGS:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........     sxmoudfa...........
				//              ^^^^^^^^------------ Allow refactoring
				//              ||||||+------------- FP64
				//              |||||+-------------- Force early depth/stencil
				//              ||||+--------------- Enable raw and structured buffers
				//              |||+---------------- Skip optimizations
				//              ||+----------------- Enable minimum precision
				//              |+------------------ Enable double extension
				//              +------------------- Enable shader extension

				_instruction.allowRefactoring       = 0 != (token & UINT32_C(0x00000800) );
				_instruction.fp64                   = 0 != (token & UINT32_C(0x00001000) );
				_instruction.earlyDepth             = 0 != (token & UINT32_C(0x00002000) );
				_instruction.enableBuffers          = 0 != (token & UINT32_C(0x00004000) );
				_instruction.skipOptimization       = 0 != (token & UINT32_C(0x00008000) );
				_instruction.enableMinPrecision     = 0 != (token & UINT32_C(0x00010000) );
				_instruction.enableDoubleExtensions = 0 != (token & UINT32_C(0x00020000) );
				_instruction.enableShaderExtensions = 0 != (token & UINT32_C(0x00040000) );
				break;

			case DxbcOpcode::DCL_INPUT_PS:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........        iiiii...........
				//                 ^---------------- Interploation

				_instruction.interpolation = DxbcInterpolation::Enum( (token & UINT32_C(0x0000f800) ) >> 11);
				break;

			case DxbcOpcode::DCL_RESOURCE:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........ sssssssrrrrr...........
				//          ^      ^---------------- SRV
				//          +----------------------- MSAA samples

				_instruction.srv     = DxbcResourceDim::Enum( (token & UINT32_C(0x0000f800) ) >> 11);
				_instruction.samples =               uint8_t( (token & UINT32_C(0x007f0000) ) >> 16);
				break;

			case DxbcOpcode::DCL_SAMPLER:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........           ms...........
				//                    ^^------------ Shadow sampler
				//                    +------------- Mono

				_instruction.shadow = 0 != (token & UINT32_C(0x00000800) );
				_instruction.mono   = 0 != (token & UINT32_C(0x00001000) );
				break;

			case DxbcOpcode::SYNC:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........         gust...........
				//                  ^^^^------------ Threads in group
				//                  ||+------------- Shared memory
				//                  |+-------------- UAV group
				//                  +--------------- UAV global

				_instruction.threadsInGroup = 0 != (token & UINT32_C(0x00000800) );
				_instruction.sharedMemory   = 0 != (token & UINT32_C(0x00001000) );
				_instruction.uavGroup       = 0 != (token & UINT32_C(0x00002000) );
				_instruction.uavGlobal      = 0 != (token & UINT32_C(0x00004000) );
				break;

			default:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// ........ ppppn    stt...........
				//          ^   ^    ^^------------- Resource info return type
				//          |   |    +-------------- Saturate
				//          |   +------------------- Test not zero
				//          +----------------------- Precise mask

				_instruction.retType  = DxbcResourceReturnType::Enum( (token & UINT32_C(0x00001800) ) >> 11);
				_instruction.saturate =                          0 != (token & UINT32_C(0x00002000) );
				_instruction.testNZ   =                          0 != (token & UINT32_C(0x00040000) );
//				_instruction.precise  =              uint8_t( (token & UINT32_C(0x00780000) ) >> 19);
				break;
		}

		_instruction.extended[0] = DxbcInstruction::ExtendedType::Count;
		for (uint32_t ii = 0; extended; ++ii)
		{
			// 0       1       2       3
			// 76543210765432107654321076543210
			// e..........................ttttt
			// ^                          ^
			// |                          +----- type
			// +-------------------------------- extended

			uint32_t extBits;
			size += bx::read(_reader, extBits);
			extended = 0 != (extBits & UINT32_C(0x80000000) );
			_instruction.extended[ii] = DxbcInstruction::ExtendedType::Enum(extBits & UINT32_C(0x0000001f) );
			_instruction.extended[ii+1] = DxbcInstruction::ExtendedType::Count;

			switch (_instruction.extended[ii])
			{
			case DxbcInstruction::ExtendedType::SampleControls:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// .          zzzzyyyyxxxx    .....
				//            ^   ^   ^
				//            |   |   +------------- x
				//            |   +----------------- y
				//            +--------------------- z

				_instruction.sampleOffsets[0] = uint8_t( (extBits & UINT32_C(0x00001e00) ) >>  9);
				_instruction.sampleOffsets[1] = uint8_t( (extBits & UINT32_C(0x0001e000) ) >> 13);
				_instruction.sampleOffsets[2] = uint8_t( (extBits & UINT32_C(0x001e0000) ) >> 17);
				break;

			case DxbcInstruction::ExtendedType::ResourceDim:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// .                          .....
				//

				_instruction.resourceTarget = uint8_t( (extBits & UINT32_C(0x000003e0) ) >>  6);
				_instruction.resourceStride = uint8_t( (extBits & UINT32_C(0x0000f800) ) >> 11);
				break;

			case DxbcInstruction::ExtendedType::ResourceReturnType:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// .          3333222211110000.....
				//            ^   ^   ^
				//            |   |   +------------- x
				//            |   +----------------- y
				//            +--------------------- z

				_instruction.resourceReturnTypes[0] = DxbcResourceReturnType::Enum( (extBits & UINT32_C(0x000001e0) ) >>   6);
				_instruction.resourceReturnTypes[1] = DxbcResourceReturnType::Enum( (extBits & UINT32_C(0x00001e00) ) >>   9);
				_instruction.resourceReturnTypes[2] = DxbcResourceReturnType::Enum( (extBits & UINT32_C(0x0001e000) ) >>  13);
				_instruction.resourceReturnTypes[3] = DxbcResourceReturnType::Enum( (extBits & UINT32_C(0x001e0000) ) >>  17);
				break;

			default:
				break;
			}
		}

		switch (_instruction.opcode)
		{
			case DxbcOpcode::DCL_FUNCTION_TABLE:
				{
					uint32_t tableId;
					size += read(_reader, tableId);

					uint32_t num;
					size += read(_reader, num);

					for (uint32_t ii = 0; ii < num; ++ii)
					{
						uint32_t bodyId;
						size += read(_reader, bodyId);
					}
				}
				break;

			case DxbcOpcode::DCL_INTERFACE:
				{
					uint32_t interfaceId;
					size += read(_reader, interfaceId);

					uint32_t num;
					size += read(_reader, num);

					BX_CHECK(false, "not implemented.");
				}
				break;

			default:
				break;
		};

		uint32_t currOp = 0;

		const DxbcOpcodeInfo& info = s_dxbcOpcodeInfo[_instruction.opcode];
		_instruction.numOperands = info.numOperands;
		switch (info.numOperands)
		{
		case 6: size += read(_reader, _instruction.operand[currOp++]);
		case 5: size += read(_reader, _instruction.operand[currOp++]);
		case 4: size += read(_reader, _instruction.operand[currOp++]);
		case 3: size += read(_reader, _instruction.operand[currOp++]);
		case 2: size += read(_reader, _instruction.operand[currOp++]);
		case 1: size += read(_reader, _instruction.operand[currOp++]);
		case 0:
			if (0 < info.numValues)
			{
				size += read(_reader, _instruction.value, info.numValues*sizeof(uint32_t) );
			}
			break;

		default:
			BX_CHECK(false, "Instruction %s with invalid number of operands %d (numValues %d)."
					, getName(_instruction.opcode)
					, info.numOperands
					, info.numValues
					);
			break;
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const DxbcInstruction& _instruction)
	{
		uint32_t token = 0;
		token |= (_instruction.opcode        ) & UINT32_C(0x000007ff);
		token |= (_instruction.length   << 24) & UINT32_C(0x7f000000);

		token |=  DxbcInstruction::ExtendedType::Count != _instruction.extended[0]
			? UINT32_C(0x80000000)
			: 0
			;

		switch (_instruction.opcode)
		{
//			case DxbcOpcode::CUSTOMDATA:
//				return size;

			case DxbcOpcode::DCL_CONSTANT_BUFFER:
				token |= _instruction.allowRefactoring ? UINT32_C(0x00000800) : 0;
				break;

			case DxbcOpcode::DCL_GLOBAL_FLAGS:
				token |= _instruction.allowRefactoring       ? UINT32_C(0x00000800) : 0;
				token |= _instruction.fp64                   ? UINT32_C(0x00001000) : 0;
				token |= _instruction.earlyDepth             ? UINT32_C(0x00002000) : 0;
				token |= _instruction.enableBuffers          ? UINT32_C(0x00004000) : 0;
				token |= _instruction.skipOptimization       ? UINT32_C(0x00008000) : 0;
				token |= _instruction.enableMinPrecision     ? UINT32_C(0x00010000) : 0;
				token |= _instruction.enableDoubleExtensions ? UINT32_C(0x00020000) : 0;
				token |= _instruction.enableShaderExtensions ? UINT32_C(0x00040000) : 0;
				break;

			case DxbcOpcode::DCL_INPUT_PS:
				token |= (_instruction.interpolation << 11) & UINT32_C(0x0000f800);
				break;

			case DxbcOpcode::DCL_RESOURCE:
				token |= (_instruction.srv     << 11) & UINT32_C(0x0000f800);
				token |= (_instruction.samples << 16) & UINT32_C(0x007f0000);
				break;

			case DxbcOpcode::DCL_SAMPLER:
				token |= _instruction.shadow ? (0x00000800) : 0;
				token |= _instruction.mono   ? (0x00001000) : 0;
				break;

			case DxbcOpcode::SYNC:
				token |= _instruction.threadsInGroup ? UINT32_C(0x00000800) : 0;
				token |= _instruction.sharedMemory   ? UINT32_C(0x00001000) : 0;
				token |= _instruction.uavGroup       ? UINT32_C(0x00002000) : 0;
				token |= _instruction.uavGlobal      ? UINT32_C(0x00004000) : 0;
				break;

			default:
				token |= (_instruction.retType << 11) & UINT32_C(0x00001800);
				token |=  _instruction.saturate ? UINT32_C(0x00002000) : 0;
				token |=  _instruction.testNZ   ? UINT32_C(0x00040000) : 0;
//				_instruction.precise  =              uint8_t( (token & UINT32_C(0x00780000) ) >> 19);
				break;
		}

		uint32_t size =0;
		size += bx::write(_writer, token);
;
		for (uint32_t ii = 0; _instruction.extended[ii] != DxbcInstruction::ExtendedType::Count; ++ii)
		{
			// 0       1       2       3
			// 76543210765432107654321076543210
			// e..........................ttttt
			// ^                          ^
			// |                          +----- type
			// +-------------------------------- extended

			token = _instruction.extended[ii+1] == DxbcInstruction::ExtendedType::Count
				? 0
				: UINT32_C(0x80000000)
				;
			token |= uint8_t(_instruction.extended[ii]);

			switch (_instruction.extended[ii])
			{
			case DxbcInstruction::ExtendedType::SampleControls:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// .          zzzzyyyyxxxx    .....
				//            ^   ^   ^
				//            |   |   +------------- x
				//            |   +----------------- y
				//            +--------------------- z

				token |= (uint32_t(_instruction.sampleOffsets[0]) <<  9) & UINT32_C(0x00001e00);
				token |= (uint32_t(_instruction.sampleOffsets[1]) << 13) & UINT32_C(0x0001e000);
				token |= (uint32_t(_instruction.sampleOffsets[2]) << 17) & UINT32_C(0x001e0000);
				break;

			case DxbcInstruction::ExtendedType::ResourceDim:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// .                          .....
				//

				token |= (uint32_t(_instruction.resourceTarget <<  6) & UINT32_C(0x000003e0) );
				token |= (uint32_t(_instruction.resourceStride << 11) & UINT32_C(0x0000f800) );
				break;

			case DxbcInstruction::ExtendedType::ResourceReturnType:
				// 0       1       2       3
				// 76543210765432107654321076543210
				// .          3333222211110000.....
				//            ^   ^   ^
				//            |   |   +------------- x
				//            |   +----------------- y
				//            +--------------------- z

				token |= (uint32_t(_instruction.resourceReturnTypes[0]) <<  6) & UINT32_C(0x000001e0);
				token |= (uint32_t(_instruction.resourceReturnTypes[1]) <<  9) & UINT32_C(0x00001e00);
				token |= (uint32_t(_instruction.resourceReturnTypes[2]) << 13) & UINT32_C(0x0001e000);
				token |= (uint32_t(_instruction.resourceReturnTypes[3]) << 17) & UINT32_C(0x001e0000);
				break;

			default:
				break;
			}

			size += bx::write(_writer, token);
		}

		for (uint32_t ii = 0; ii < _instruction.numOperands; ++ii)
		{
			size += write(_writer, _instruction.operand[ii]);
		}

		const DxbcOpcodeInfo& info = s_dxbcOpcodeInfo[_instruction.opcode];
		if (0 < info.numValues)
		{
			size += bx::write(_writer, _instruction.value, info.numValues*sizeof(uint32_t) );
		}

		return size;
	}

	int32_t toString(char* _out, int32_t _size, const DxbcInstruction& _instruction)
	{
		int32_t size = 0;

		size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
							, "%s%s%s"
							, getName(_instruction.opcode)
							, _instruction.saturate ? "_sat" : ""
							, _instruction.testNZ ? "_nz" : ""
							);

		if (DxbcResourceDim::Unknown != _instruction.srv)
		{
			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
								, " %s<%x>"
								, s_dxbcSrvType[_instruction.srv]
								, _instruction.value[0]
								);
		}
		else if (0 < s_dxbcOpcodeInfo[_instruction.opcode].numValues)
		{
			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
								, " %d"
								, _instruction.value[0]
								);
		}

		for (uint32_t ii = 0; ii < _instruction.numOperands; ++ii)
		{
			const DxbcOperand& operand = _instruction.operand[ii];

			const bool array = false
				|| 1 < operand.numAddrModes
				|| DxbcOperandAddrMode::Imm32 != operand.addrMode[0]
				;

			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
								, "%s%s%s"
								, 0 == ii ? " " : ", "
								, operand.extended ? "*" : ""
								, s_dxbcOperandType[operand.type]
								);

			switch (operand.type)
			{
			case DxbcOperandType::Imm32:
			case DxbcOperandType::Imm64:
				for (uint32_t jj = 0; jj < operand.num; ++jj)
				{
					union { uint32_t i; float f; } cast = { operand.un.imm32[jj] };
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "%s%f"
										, 0 == jj ? "(" : ", "
										, cast.f
										);
				}

				size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
									, ")"
									);
				break;

			default:
				break;
			}

			const uint32_t first = DxbcOperandAddrMode::RegImm32 == operand.addrMode[0] ? 0 : 1;
			if (0 == first)
			{
				size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
									, "["
									);
			}
			else
			{
				size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
									, "%d%s"
									, operand.regIndex[0]
									, array ? "[" : ""
									);
			}

			for (uint32_t jj = first; jj < operand.numAddrModes; ++jj)
			{
				switch (operand.addrMode[jj])
				{
				case DxbcOperandAddrMode::Imm32:
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "%d"
										, operand.regIndex[jj]
										);
					break;

				case DxbcOperandAddrMode::Reg:
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "%s%d"
										, s_dxbcOperandType[operand.subOperand[jj].type]
										, operand.regIndex[jj]
										);
					break;

				case DxbcOperandAddrMode::RegImm32:
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, "%d + %s%d"
										, operand.regIndex[jj]
										, s_dxbcOperandType[operand.subOperand[jj].type]
										, operand.regIndex[jj]
										);
					break;

				default:
					break;
				}
			}

			size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
								, "%s"
								, array ? "]" : ""
								);

			switch (operand.mode)
			{
			case DxbcOperandMode::Mask:
				if (0xf > operand.modeBits
				&&  0   < operand.modeBits)
				{
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, ".%s%s%s%s"
										, 0 == (operand.modeBits & 1) ? "" : "x"
										, 0 == (operand.modeBits & 2) ? "" : "y"
										, 0 == (operand.modeBits & 4) ? "" : "z"
										, 0 == (operand.modeBits & 8) ? "" : "w"
										);
				}
				break;

			case DxbcOperandMode::Swizzle:
				if (0xe4 != operand.modeBits)
				{
					size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
										, ".%c%c%c%c"
										, "xyzw"[(operand.modeBits   )&0x3]
										, "xyzw"[(operand.modeBits>>2)&0x3]
										, "xyzw"[(operand.modeBits>>4)&0x3]
										, "xyzw"[(operand.modeBits>>6)&0x3]
										);
				}
				break;

			case DxbcOperandMode::Scalar:
				size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
									, ".%c"
									, "xyzw"[operand.modeBits]
									);
				break;

			default:
				break;
			}

		}

		return size;
	}

	int32_t read(bx::ReaderSeekerI* _reader, DxbcSignature& _signature)
	{
		int32_t size = 0;

		int64_t offset = bx::seek(_reader);

		uint32_t num;
		size += bx::read(_reader, num);
		size += bx::read(_reader, _signature.key);

		for (uint32_t ii = 0; ii < num; ++ii)
		{
			DxbcSignature::Element element;

			uint32_t nameOffset;
			size += bx::read(_reader, nameOffset);

			char name[DXBC_MAX_NAME_STRING];
			readString(_reader, offset + nameOffset, name);
			element.name = name;

			size += bx::read(_reader, element.semanticIndex);
			size += bx::read(_reader, element.valueType);
			size += bx::read(_reader, element.componentType);
			size += bx::read(_reader, element.registerIndex);
			size += bx::read(_reader, element.mask);
			size += bx::read(_reader, element.readWriteMask);
			size += bx::read(_reader, element.stream);

			// padding
			uint8_t padding;
			size += bx::read(_reader, padding);

			_signature.elements.push_back(element);
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const DxbcSignature& _signature)
	{
		int32_t size = 0;

		const uint32_t num = uint32_t(_signature.elements.size() );
		size += bx::write(_writer, num);
		size += bx::write(_writer, _signature.key);

		typedef stl::unordered_map<stl::string, uint32_t> NameOffsetMap;
		NameOffsetMap nom;

		const uint8_t pad = 0;
		uint32_t nameOffset = num * 24 + 8;
		for (uint32_t ii = 0; ii < num; ++ii)
		{
			const DxbcSignature::Element& element = _signature.elements[ii];

			NameOffsetMap::iterator it = nom.find(element.name);
			if (it == nom.end() )
			{
				nom.insert(stl::make_pair(element.name, nameOffset) );
				size += bx::write(_writer, nameOffset);
				nameOffset += uint32_t(element.name.size() + 1);
			}
			else
			{
				size += bx::write(_writer, it->second);
			}

			size += bx::write(_writer, element.semanticIndex);
			size += bx::write(_writer, element.valueType);
			size += bx::write(_writer, element.componentType);
			size += bx::write(_writer, element.registerIndex);
			size += bx::write(_writer, element.mask);
			size += bx::write(_writer, element.readWriteMask);
			size += bx::write(_writer, element.stream);
			size += bx::write(_writer, pad);

		}

		uint32_t len = 0;
		for (uint32_t ii = 0; ii < num; ++ii)
		{
			const DxbcSignature::Element& element = _signature.elements[ii];
			NameOffsetMap::iterator it = nom.find(element.name);
			if (it != nom.end() )
			{
				nom.erase(it);
				size += bx::write(_writer, element.name.c_str(), uint32_t(element.name.size() + 1) );
				len  += uint32_t(element.name.size() + 1);
			}
		}

		// align 4 bytes
		size += bx::writeRep(_writer, 0xab, (len+3)/4*4 - len);

		return size;
	}

	int32_t read(bx::ReaderSeekerI* _reader, DxbcShader& _shader)
	{
		int32_t size = 0;

		size += bx::read(_reader, _shader.version);

		uint32_t bcLength;
		size += bx::read(_reader, bcLength);

		uint32_t len = (bcLength-2)*sizeof(uint32_t);
		_shader.byteCode.resize(len);
		size += bx::read(_reader, _shader.byteCode.data(), len);

		return size;
	}

	int32_t write(bx::WriterI* _writer, const DxbcShader& _shader)
	{
		const uint32_t len = uint32_t(_shader.byteCode.size() );
		const uint32_t bcLength = len / sizeof(uint32_t) + 2;

		int32_t size = 0;
		size += bx::write(_writer, _shader.version);
		size += bx::write(_writer, bcLength);
		size += bx::write(_writer, _shader.byteCode.data(), len);

		return size;
	}

#define DXBC_CHUNK_HEADER           BX_MAKEFOURCC('D', 'X', 'B', 'C')
#define DXBC_CHUNK_SHADER           BX_MAKEFOURCC('S', 'H', 'D', 'R')
#define DXBC_CHUNK_SHADER_EX        BX_MAKEFOURCC('S', 'H', 'E', 'X')

#define DXBC_CHUNK_INPUT_SIGNATURE  BX_MAKEFOURCC('I', 'S', 'G', 'N')
#define DXBC_CHUNK_OUTPUT_SIGNATURE BX_MAKEFOURCC('O', 'S', 'G', 'N')

	int32_t read(bx::ReaderSeekerI* _reader, DxbcContext& _dxbc)
	{
		int32_t size = 0;
		size += bx::read(_reader, _dxbc.header);
		_dxbc.shader.shex = false;

		for (uint32_t ii = 0; ii < _dxbc.header.numChunks; ++ii)
		{
			bx::seek(_reader, sizeof(DxbcContext::Header) + ii*sizeof(uint32_t), bx::Whence::Begin);

			uint32_t chunkOffset;
			size += bx::read(_reader, chunkOffset);

			bx::seek(_reader, chunkOffset, bx::Whence::Begin);

			uint32_t fourcc;
			size += bx::read(_reader, fourcc);

			uint32_t chunkSize;
			size += bx::read(_reader, chunkSize);

			switch (fourcc)
			{
			case DXBC_CHUNK_SHADER_EX:
				_dxbc.shader.shex = true;
				// fallthrough

			case DXBC_CHUNK_SHADER:
				size += read(_reader, _dxbc.shader);
				break;

			case BX_MAKEFOURCC('I', 'S', 'G', '1'):
			case DXBC_CHUNK_INPUT_SIGNATURE:
				size += read(_reader, _dxbc.inputSignature);
				break;

			case BX_MAKEFOURCC('O', 'S', 'G', '1'):
			case BX_MAKEFOURCC('O', 'S', 'G', '5'):
			case DXBC_CHUNK_OUTPUT_SIGNATURE:
				size += read(_reader, _dxbc.outputSignature);
				break;

			case BX_MAKEFOURCC('R', 'D', 'E', 'F'):
			case BX_MAKEFOURCC('I', 'F', 'C', 'E'):
			case BX_MAKEFOURCC('P', 'C', 'S', 'G'):
			case BX_MAKEFOURCC('S', 'T', 'A', 'T'):
			case BX_MAKEFOURCC('S', 'F', 'I', '0'):
			case BX_MAKEFOURCC('P', 'S', 'O', '1'):
			case BX_MAKEFOURCC('P', 'S', 'O', '2'):
				size += chunkSize;
				break;

			default:
				size += chunkSize;
				BX_CHECK(false, "UNKNOWN FOURCC %c%c%c%c %d"
					, ( (char*)&fourcc)[0]
					, ( (char*)&fourcc)[1]
					, ( (char*)&fourcc)[2]
					, ( (char*)&fourcc)[3]
					, size
					);
				break;
			}
		}

		return size;
	}

	int32_t write(bx::WriterSeekerI* _writer, const DxbcContext& _dxbc)
	{
		int32_t size = 0;

		int64_t dxbcOffset = bx::seek(_writer);
		size += bx::write(_writer, DXBC_CHUNK_HEADER);

		size += bx::writeRep(_writer, 0, 16);

		size += bx::write(_writer, UINT32_C(1) );

		int64_t sizeOffset = bx::seek(_writer);
		size += bx::writeRep(_writer, 0, 4);

		uint32_t numChunks = 3;
		size += bx::write(_writer, numChunks);

		int64_t chunksOffsets = bx::seek(_writer);
		size += bx::writeRep(_writer, 0, numChunks*sizeof(uint32_t) );

		uint32_t chunkOffset[3];
		uint32_t chunkSize[3];

		chunkOffset[0] = uint32_t(bx::seek(_writer) - dxbcOffset);
		size += write(_writer, DXBC_CHUNK_INPUT_SIGNATURE);
		size += write(_writer, UINT32_C(0) );
		chunkSize[0] = write(_writer, _dxbc.inputSignature);

		chunkOffset[1] = uint32_t(bx::seek(_writer) - dxbcOffset);
		size += write(_writer, DXBC_CHUNK_OUTPUT_SIGNATURE);
		size += write(_writer, UINT32_C(0) );
		chunkSize[1] = write(_writer, _dxbc.outputSignature);

		chunkOffset[2] = uint32_t(bx::seek(_writer) - dxbcOffset);
		size += write(_writer, _dxbc.shader.shex ? DXBC_CHUNK_SHADER_EX : DXBC_CHUNK_SHADER);
		size += write(_writer, UINT32_C(0) );
		chunkSize[2] = write(_writer, _dxbc.shader);

		size += 0
			+ chunkSize[0]
			+ chunkSize[1]
			+ chunkSize[2]
			;

		int64_t eof = bx::seek(_writer);

		bx::seek(_writer, sizeOffset, bx::Whence::Begin);
		bx::write(_writer, size);

		bx::seek(_writer, chunksOffsets, bx::Whence::Begin);
		bx::write(_writer, chunkOffset, sizeof(chunkOffset) );

		for (uint32_t ii = 0; ii < BX_COUNTOF(chunkOffset); ++ii)
		{
			bx::seek(_writer, chunkOffset[ii]+4, bx::Whence::Begin);
			bx::write(_writer, chunkSize[ii]);
		}

		bx::seek(_writer, eof, bx::Whence::Begin);

		return size;
	}

	void parse(const DxbcShader& _src, DxbcParseFn _fn, void* _userData)
	{
		bx::MemoryReader reader(_src.byteCode.data(), uint32_t(_src.byteCode.size() ) );

		for (uint32_t token = 0, numTokens = uint32_t(_src.byteCode.size() / sizeof(uint32_t) ); token < numTokens;)
		{
			DxbcInstruction instruction;
			uint32_t size = read(&reader, instruction);
			BX_CHECK(size/4 == instruction.length, "read %d, expected %d", size/4, instruction.length); BX_UNUSED(size);

			_fn(token * sizeof(uint32_t), instruction, _userData);

			token += instruction.length;
		}
	}

	void filter(DxbcShader& _dst, const DxbcShader& _src, DxbcFilterFn _fn, void* _userData)
	{
		bx::MemoryReader reader(_src.byteCode.data(), uint32_t(_src.byteCode.size() ) );

		bx::CrtAllocator r;
		bx::MemoryBlock mb(&r);
		bx::MemoryWriter writer(&mb);

		for (uint32_t token = 0, numTokens = uint32_t(_src.byteCode.size() / sizeof(uint32_t) ); token < numTokens;)
		{
			DxbcInstruction instruction;
			uint32_t size = read(&reader, instruction);
			BX_CHECK(size/4 == instruction.length, "read %d, expected %d", size/4, instruction.length); BX_UNUSED(size);

			_fn(instruction, _userData);

			write(&writer, instruction);

			token += instruction.length;
		}

		uint8_t* data = (uint8_t*)mb.more();
		uint32_t size = uint32_t(bx::getSize(&writer) );
		_dst.byteCode.reserve(size);
		memcpy(_dst.byteCode.data(), data, size);
	}

} // namespace bgfx
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include "bgfx_p.h"
#include "shader_spirv.h"

namespace bgfx
{
	struct SpirvOpcodeInfo
	{
		uint8_t numOperands;
		uint8_t numValues;
		bool    hasVariable;
	};

	static const SpirvOpcodeInfo s_sprivOpcodeInfo[] =
	{
		{ 0, 0, false }, // Nop,
		{ 0, 0, true  }, // Source
		{ 0, 0, true  }, // SourceExtension
		{ 0, 0, false }, // Extension
		{ 0, 1, true  }, // ExtInstImport
		{ 0, 2, false }, // MemoryModel
		{ 0, 2, false }, // EntryPoint
		{ 0, 0, false }, // ExecutionMode
		{ 0, 1, false }, // TypeVoid
		{ 0, 1, false }, // TypeBool
		{ 0, 3, false }, // TypeInt
		{ 0, 2, false }, // TypeFloat
		{ 0, 3, false }, // TypeVector
		{ 0, 3, false }, // TypeMatrix
		{ 1, 7, false }, // TypeSampler
		{ 0, 0, false }, // TypeFilter
		{ 0, 0, false }, // TypeArray
		{ 0, 0, false }, // TypeRuntimeArray
		{ 0, 0, false }, // TypeStruct
		{ 0, 0, false }, // TypeOpaque
		{ 0, 3, false }, // TypePointer
		{ 0, 2, true  }, // TypeFunction
		{ 0, 0, false }, // TypeEvent
		{ 0, 0, false }, // TypeDeviceEvent
		{ 0, 0, false }, // TypeReserveId
		{ 0, 0, false }, // TypeQueue
		{ 0, 0, false }, // TypePipe
		{ 0, 0, false }, // ConstantTrue
		{ 0, 0, false }, // ConstantFalse
		{ 0, 2, true  }, // Constant
		{ 0, 2, true  }, // ConstantComposite
		{ 0, 0, false }, // ConstantSampler
		{ 0, 0, false }, // ConstantNullPointer
		{ 0, 0, false }, // ConstantNullObject
		{ 0, 0, false }, // SpecConstantTrue
		{ 0, 0, false }, // SpecConstantFalse
		{ 0, 0, false }, // SpecConstant
		{ 0, 0, false }, // SpecConstantComposite
		{ 0, 3, true  }, // Variable
		{ 0, 0, false }, // VariableArray
		{ 0, 4, false }, // Function
		{ 0, 0, false }, // FunctionParameter
		{ 0, 0, false }, // FunctionEnd
		{ 0, 0, false }, // FunctionCall
		{ 0, 0, false }, // ExtInst
		{ 0, 0, false }, // Undef
		{ 0, 0, false }, // Load
		{ 0, 2, true  }, // Store
		{ 0, 0, false }, // Phi
		{ 0, 0, false }, // DecorationGroup
		{ 0, 2, true  }, // Decorate
		{ 0, 0, false }, // MemberDecorate
		{ 0, 0, false }, // GroupDecorate
		{ 0, 0, false }, // GroupMemberDecorate
		{ 0, 1, true  }, // Name
		{ 0, 1, true  }, // MemberName
		{ 0, 0, false }, // String
		{ 0, 0, false }, // Line
		{ 0, 0, false }, // VectorExtractDynamic
		{ 0, 0, false }, // VectorInsertDynamic
		{ 0, 0, false }, // VectorShuffle
		{ 0, 0, false }, // CompositeConstruct
		{ 0, 0, false }, // CompositeExtract
		{ 0, 0, false }, // CompositeInsert
		{ 0, 0, false }, // CopyObject
		{ 0, 0, false }, // CopyMemory
		{ 0, 0, false }, // CopyMemorySized
		{ 0, 0, false }, // Sampler
		{ 0, 0, false }, // TextureSample
		{ 0, 0, false }, // TextureSampleDref
		{ 0, 0, false }, // TextureSampleLod
		{ 0, 0, false }, // TextureSampleProj
		{ 0, 0, false }, // TextureSampleGrad
		{ 0, 0, false }, // TextureSampleOffset
		{ 0, 0, false }, // TextureSampleProjLod
		{ 0, 0, false }, // TextureSampleProjGrad
		{ 0, 0, false }, // TextureSampleLodOffset
		{ 0, 0, false }, // TextureSampleProjOffset
		{ 0, 0, false }, // TextureSampleGradOffset
		{ 0, 0, false }, // TextureSampleProjLodOffset
		{ 0, 0, false }, // TextureSampleProjGradOffset
		{ 0, 0, false }, // TextureFetchTexelLod
		{ 0, 0, false }, // TextureFetchTexelOffset
		{ 0, 0, false }, // TextureFetchSample
		{ 0, 0, false }, // TextureFetchTexel
		{ 0, 0, false }, // TextureGather
		{ 0, 0, false }, // TextureGatherOffset
		{ 0, 0, false }, // TextureGatherOffsets
		{ 0, 0, false }, // TextureQuerySizeLod
		{ 0, 0, false }, // TextureQuerySize
		{ 0, 0, false }, // TextureQueryLod
		{ 0, 0, false }, // TextureQueryLevels
		{ 0, 0, false }, // TextureQuerySamples
		{ 0, 0, false }, // AccessChain
		{ 0, 0, false }, // InBoundsAccessChain
		{ 0, 0, false }, // SNegate
		{ 0, 0, false }, // FNegate
		{ 0, 0, false }, // Not
		{ 0, 0, false }, // Any
		{ 0, 0, false }, // All
		{ 0, 0, false }, // ConvertFToU
		{ 0, 0, false }, // ConvertFToS
		{ 0, 0, false }, // ConvertSToF
		{ 0, 0, false }, // ConvertUToF
		{ 0, 0, false }, // UConvert
		{ 0, 0, false }, // SConvert
		{ 0, 0, false }, // FConvert
		{ 0, 0, false }, // ConvertPtrToU
		{ 0, 0, false }, // ConvertUToPtr
		{ 0, 0, false }, // PtrCastToGeneric
		{ 0, 0, false }, // GenericCastToPtr
		{ 0, 0, false }, // Bitcast
		{ 0, 0, false }, // Transpose
		{ 0, 0, false }, // IsNan
		{ 0, 0, false }, // IsInf
		{ 0, 0, false }, // IsFinite
		{ 0, 0, false }, // IsNormal
		{ 0, 0, false }, // SignBitSet
		{ 0, 0, false }, // LessOrGreater
		{ 0, 0, false }, // Ordered
		{ 0, 0, false }, // Unordered
		{ 0, 0, false }, // ArrayLength
		{ 0, 0, false }, // IAdd
		{ 0, 0, false }, // FAdd
		{ 0, 0, false }, // ISub
		{ 0, 0, false }, // FSub
		{ 0, 0, false }, // IMul
		{ 0, 0, false }, // FMul
		{ 0, 0, false }, // UDiv
		{ 0, 0, false }, // SDiv
		{ 0, 0, false }, // FDiv
		{ 0, 0, false }, // UMod
		{ 0, 0, false }, // SRem
		{ 0, 0, false }, // SMod
		{ 0, 0, false }, // FRem
		{ 0, 0, false }, // FMod
		{ 0, 0, false }, // VectorTimesScalar
		{ 0, 0, false }, // MatrixTimesScalar
		{ 0, 0, false }, // VectorTimesMatrix
		{ 0, 0, false }, // MatrixTimesVector
		{ 0, 0, false }, // MatrixTimesMatrix
		{ 0, 0, false }, // OuterProduct
		{ 0, 0, false }, // Dot
		{ 0, 0, false }, // ShiftRightLogical
		{ 0, 0, false }, // ShiftRightArithmetic
		{ 0, 0, false }, // ShiftLeftLogical
		{ 0, 0, false }, // LogicalOr
		{ 0, 0, false }, // LogicalXor
		{ 0, 0, false }, // LogicalAnd
		{ 0, 0, false }, // BitwiseOr
		{ 0, 0, false }, // BitwiseXor
		{ 0, 0, false }, // BitwiseAnd
		{ 0, 0, false }, // Select
		{ 0, 0, false }, // IEqual
		{ 0, 0, false }, // FOrdEqual
		{ 0, 0, false }, // FUnordEqual
		{ 0, 0, false }, // INotEqual
		{ 0, 0, false }, // FOrdNotEqual
		{ 0, 0, false }, // FUnordNotEqual
		{ 0, 0, false }, // ULessThan
		{ 0, 0, false }, // SLessThan
		{ 0, 0, false }, // FOrdLessThan
		{ 0, 0, false }, // FUnordLessThan
		{ 0, 0, false }, // UGreaterThan
		{ 0, 0, false }, // SGreaterThan
		{ 0, 0, false }, // FOrdGreaterThan
		{ 0, 0, false }, // FUnordGreaterThan
		{ 0, 0, false }, // ULessThanEqual
		{ 0, 0, false }, // SLessThanEqual
		{ 0, 0, false }, // FOrdLessThanEqual
		{ 0, 0, false }, // FUnordLessThanEqual
		{ 0, 0, false }, // UGreaterThanEqual
		{ 0, 0, false }, // SGreaterThanEqual
		{ 0, 0, false }, // FOrdGreaterThanEqual
		{ 0, 0, false }, // FUnordGreaterThanEqual
		{ 0, 0, false }, // DPdx
		{ 0, 0, false }, // DPdy
		{ 0, 0, false }, // Fwidth
		{ 0, 0, false }, // DPdxFine
		{ 0, 0, false }, // DPdyFine
		{ 0, 0, false }, // FwidthFine
		{ 0, 0, false }, // DPdxCoarse
		{ 0, 0, false }, // DPdyCoarse
		{ 0, 0, false }, // FwidthCoarse
		{ 0, 0, false }, // EmitVertex
		{ 0, 0, false }, // EndPrimitive
		{ 0, 0, false }, // EmitStreamVertex
		{ 0, 0, false }, // EndStreamPrimitive
		{ 0, 0, false }, // ControlBarrier
		{ 0, 0, false }, // MemoryBarrier
		{ 0, 0, false }, // ImagePointer
		{ 0, 0, false }, // AtomicInit
		{ 0, 0, false }, // AtomicLoad
		{ 0, 0, false }, // AtomicStore
		{ 0, 0, false }, // AtomicExchange
		{ 0, 0, false }, // AtomicCompareExchange
		{ 0, 0, false }, // AtomicCompareExchangeWeak
		{ 0, 0, false }, // AtomicIIncrement
		{ 0, 0, false }, // AtomicIDecrement
		{ 0, 0, false }, // AtomicIAdd
		{ 0, 0, false }, // AtomicISub
		{ 0, 0, false }, // AtomicUMin
		{ 0, 0, false }, // AtomicUMax
		{ 0, 0, false }, // AtomicAnd
		{ 0, 0, false }, // AtomicOr
		{ 0, 0, false }, // AtomicXor
		{ 0, 0, false }, // LoopMerge
		{ 0, 0, false }, // SelectionMerge
		{ 0, 1, false }, // Label
		{ 0, 1, false }, // Branch
		{ 0, 0, false }, // BranchConditional
		{ 0, 0, false }, // Switch
		{ 0, 0, false }, // Kill
		{ 0, 0, false }, // Return
		{ 0, 0, false }, // ReturnValue
		{ 0, 0, false }, // Unreachable
		{ 0, 0, false }, // LifetimeStart
		{ 0, 0, false }, // LifetimeStop
		{ 0, 0, false }, // CompileFlag
		{ 0, 0, false }, // AsyncGroupCopy
		{ 0, 0, false }, // WaitGroupEvents
		{ 0, 0, false }, // GroupAll
		{ 0, 0, false }, // GroupAny
		{ 0, 0, false }, // GroupBroadcast
		{ 0, 0, false }, // GroupIAdd
		{ 0, 0, false }, // GroupFAdd
		{ 0, 0, false }, // GroupFMin
		{ 0, 0, false }, // GroupUMin
		{ 0, 0, false }, // GroupSMin
		{ 0, 0, false }, // GroupFMax
		{ 0, 0, false }, // GroupUMax
		{ 0, 0, false }, // GroupSMax
		{ 0, 0, false }, // GenericCastToPtrExplicit
		{ 0, 0, false }, // GenericPtrMemSemantics
		{ 0, 0, false }, // ReadPipe
		{ 0, 0, false }, // WritePipe
		{ 0, 0, false }, // ReservedReadPipe
		{ 0, 0, false }, // ReservedWritePipe
		{ 0, 0, false }, // ReserveReadPipePackets
		{ 0, 0, false }, // ReserveWritePipePackets
		{ 0, 0, false }, // CommitReadPipe
		{ 0, 0, false }, // CommitWritePipe
		{ 0, 0, false }, // IsValidReserveId
		{ 0, 0, false }, // GetNumPipePackets
		{ 0, 0, false }, // GetMaxPipePackets
		{ 0, 0, false }, // GroupReserveReadPipePackets
		{ 0, 0, false }, // GroupReserveWritePipePackets
		{ 0, 0, false }, // GroupCommitReadPipe
		{ 0, 0, false }, // GroupCommitWritePipe
		{ 0, 0, false }, // EnqueueMarker
		{ 0, 0, false }, // EnqueueKernel
		{ 0, 0, false }, // GetKernelNDrangeSubGroupCount
		{ 0, 0, false }, // GetKernelNDrangeMaxSubGroupSize
		{ 0, 0, false }, // GetKernelWorkGroupSize
		{ 0, 0, false }, // GetKernelPreferredWorkGroupSizeMultiple
		{ 0, 0, false }, // RetainEvent
		{ 0, 0, false }, // ReleaseEvent
		{ 0, 0, false }, // CreateUserEvent
		{ 0, 0, false }, // IsValidEvent
		{ 0, 0, false }, // SetUserEventStatus
		{ 0, 0, false }, // CaptureEventProfilingInfo
		{ 0, 0, false }, // GetDefaultQueue
		{ 0, 0, false }, // BuildNDRange
		{ 0, 0, false }, // SatConvertSToU
		{ 0, 0, false }, // SatConvertUToS
		{ 0, 0, false }, // AtomicIMin
		{ 0, 0, false }, // AtomicIMax
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_sprivOpcodeInfo) == SpirvOpcode::Count);

	const char* s_spirvOpcode[] =
	{
		"Nop",
		"Source",
		"SourceExtension",
		"Extension",
		"ExtInstImport",
		"MemoryModel",
		"EntryPoint",
		"ExecutionMode",
		"TypeVoid",
		"TypeBool",
		"TypeInt",
		"TypeFloat",
		"TypeVector",
		"TypeMatrix",
		"TypeSampler",
		"TypeFilter",
		"TypeArray",
		"TypeRuntimeArray",
		"TypeStruct",
		"TypeOpaque",
		"TypePointer",
		"TypeFunction",
		"TypeEvent",
		"TypeDeviceEvent",
		"TypeReserveId",
		"TypeQueue",
		"TypePipe",
		"ConstantTrue",
		"ConstantFalse",
		"Constant",
		"ConstantComposite",
		"ConstantSampler",
		"ConstantNullPointer",
		"ConstantNullObject",
		"SpecConstantTrue",
		"SpecConstantFalse",
		"SpecConstant",
		"SpecConstantComposite",
		"Variable",
		"VariableArray",
		"Function",
		"FunctionParameter",
		"FunctionEnd",
		"FunctionCall",
		"ExtInst",
		"Undef",
		"Load",
		"Store",
		"Phi",
		"DecorationGroup",
		"Decorate",
		"MemberDecorate",
		"GroupDecorate",
		"GroupMemberDecorate",
		"Name",
		"MemberName",
		"String",
		"Line",
		"VectorExtractDynamic",
		"VectorInsertDynamic",
		"VectorShuffle",
		"CompositeConstruct",
		"CompositeExtract",
		"CompositeInsert",
		"CopyObject",
		"CopyMemory",
		"CopyMemorySized",
		"Sampler",
		"TextureSample",
		"TextureSampleDref",
		"TextureSampleLod",
		"TextureSampleProj",
		"TextureSampleGrad",
		"TextureSampleOffset",
		"TextureSampleProjLod",
		"TextureSampleProjGrad",
		"TextureSampleLodOffset",
		"TextureSampleProjOffset",
		"TextureSampleGradOffset",
		"TextureSampleProjLodOffset",
		"TextureSampleProjGradOffset",
		"TextureFetchTexelLod",
		"TextureFetchTexelOffset",
		"TextureFetchSample",
		"TextureFetchTexel",
		"TextureGather",
		"TextureGatherOffset",
		"TextureGatherOffsets",
		"TextureQuerySizeLod",
		"TextureQuerySize",
		"TextureQueryLod",
		"TextureQueryLevels",
		"TextureQuerySamples",
		"AccessChain",
		"InBoundsAccessChain",
		"SNegate",
		"FNegate",
		"Not",
		"Any",
		"All",
		"ConvertFToU",
		"ConvertFToS",
		"ConvertSToF",
		"ConvertUToF",
		"UConvert",
		"SConvert",
		"FConvert",
		"ConvertPtrToU",
		"ConvertUToPtr",
		"PtrCastToGeneric",
		"GenericCastToPtr",
		"Bitcast",
		"Transpose",
		"IsNan",
		"IsInf",
		"IsFinite",
		"IsNormal",
		"SignBitSet",
		"LessOrGreater",
		"Ordered",
		"Unordered",
		"ArrayLength",
		"IAdd",
		"FAdd",
		"ISub",
		"FSub",
		"IMul",
		"FMul",
		"UDiv",
		"SDiv",
		"FDiv",
		"UMod",
		"SRem",
		"SMod",
		"FRem",
		"FMod",
		"VectorTimesScalar",
		"MatrixTimesScalar",
		"VectorTimesMatrix",
		"MatrixTimesVector",
		"MatrixTimesMatrix",
		"OuterProduct",
		"Dot",
		"ShiftRightLogical",
		"ShiftRightArithmetic",
		"ShiftLeftLogical",
		"LogicalOr",
		"LogicalXor",
		"LogicalAnd",
		"BitwiseOr",
		"BitwiseXor",
		"BitwiseAnd",
		"Select",
		"IEqual",
		"FOrdEqual",
		"FUnordEqual",
		"INotEqual",
		"FOrdNotEqual",
		"FUnordNotEqual",
		"ULessThan",
		"SLessThan",
		"FOrdLessThan",
		"FUnordLessThan",
		"UGreaterThan",
		"SGreaterThan",
		"FOrdGreaterThan",
		"FUnordGreaterThan",
		"ULessThanEqual",
		"SLessThanEqual",
		"FOrdLessThanEqual",
		"FUnordLessThanEqual",
		"UGreaterThanEqual",
		"SGreaterThanEqual",
		"FOrdGreaterThanEqual",
		"FUnordGreaterThanEqual",
		"DPdx",
		"DPdy",
		"Fwidth",
		"DPdxFine",
		"DPdyFine",
		"FwidthFine",
		"DPdxCoarse",
		"DPdyCoarse",
		"FwidthCoarse",
		"EmitVertex",
		"EndPrimitive",
		"EmitStreamVertex",
		"EndStreamPrimitive",
		"ControlBarrier",
		"MemoryBarrier",
		"ImagePointer",
		"AtomicInit",
		"AtomicLoad",
		"AtomicStore",
		"AtomicExchange",
		"AtomicCompareExchange",
		"AtomicCompareExchangeWeak",
		"AtomicIIncrement",
		"AtomicIDecrement",
		"AtomicIAdd",
		"AtomicISub",
		"AtomicUMin",
		"AtomicUMax",
		"AtomicAnd",
		"AtomicOr",
		"AtomicXor",
		"LoopMerge",
		"SelectionMerge",
		"Label",
		"Branch",
		"BranchConditional",
		"Switch",
		"Kill",
		"Return",
		"ReturnValue",
		"Unreachable",
		"LifetimeStart",
		"LifetimeStop",
		"CompileFlag",
		"AsyncGroupCopy",
		"WaitGroupEvents",
		"GroupAll",
		"GroupAny",
		"GroupBroadcast",
		"GroupIAdd",
		"GroupFAdd",
		"GroupFMin",
		"GroupUMin",
		"GroupSMin",
		"GroupFMax",
		"GroupUMax",
		"GroupSMax",
		"GenericCastToPtrExplicit",
		"GenericPtrMemSemantics",
		"ReadPipe",
		"WritePipe",
		"ReservedReadPipe",
		"ReservedWritePipe",
		"ReserveReadPipePackets",
		"ReserveWritePipePackets",
		"CommitReadPipe",
		"CommitWritePipe",
		"IsValidReserveId",
		"GetNumPipePackets",
		"GetMaxPipePackets",
		"GroupReserveReadPipePackets",
		"GroupReserveWritePipePackets",
		"GroupCommitReadPipe",
		"GroupCommitWritePipe",
		"EnqueueMarker",
		"EnqueueKernel",
		"GetKernelNDrangeSubGroupCount",
		"GetKernelNDrangeMaxSubGroupSize",
		"GetKernelWorkGroupSize",
		"GetKernelPreferredWorkGroupSizeMultiple",
		"RetainEvent",
		"ReleaseEvent",
		"CreateUserEvent",
		"IsValidEvent",
		"SetUserEventStatus",
		"CaptureEventProfilingInfo",
		"GetDefaultQueue",
		"BuildNDRange",
		"SatConvertSToU",
		"SatConvertUToS",
		"AtomicIMin",
		"AtomicIMax",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_spirvOpcode) == SpirvOpcode::Count);

	const char* getName(SpirvOpcode::Enum _opcode)
	{
		BX_CHECK(_opcode < SpirvOpcode::Count, "Unknown opcode id %d.", _opcode);
		return s_spirvOpcode[_opcode];
	}

	int32_t read(bx::ReaderI* _reader, SpirvOperand& _operand)
	{
		int32_t size = 0;

		BX_UNUSED(_operand);
		uint32_t token;
		size += bx::read(_reader, token);

		return size;
	}

	int32_t read(bx::ReaderI* _reader, SpirvInstruction& _instruction)
	{
		int32_t size = 0;

		uint32_t token;
		size += bx::read(_reader, token);

		_instruction.opcode = SpirvOpcode::Enum( (token & UINT32_C(0x0000ffff) )      );
		_instruction.length =          uint16_t( (token & UINT32_C(0xffff0000) ) >> 16);

		uint32_t currOp = 0;

		const SpirvOpcodeInfo& info = s_sprivOpcodeInfo[_instruction.opcode];

		if (0 < info.numValues)
		{
			size += read(_reader, _instruction.un.value, info.numValues*sizeof(uint32_t) );
		}

		if (info.hasVariable)
		{
			while (size/4 != _instruction.length)
			{
				uint32_t tmp;
				size += bx::read(_reader, tmp);
			}
		}
		else
		{
			_instruction.numOperands = info.numOperands;
			switch (info.numOperands)
			{
			case 6: size += read(_reader, _instruction.operand[currOp++]);
			case 5: size += read(_reader, _instruction.operand[currOp++]);
			case 4: size += read(_reader, _instruction.operand[currOp++]);
			case 3: size += read(_reader, _instruction.operand[currOp++]);
			case 2: size += read(_reader, _instruction.operand[currOp++]);
			case 1: size += read(_reader, _instruction.operand[currOp++]);
			case 0:
				break;

			default:
				BX_WARN(false, "Instruction %s with invalid number of operands %d (numValues %d)."
						, getName(_instruction.opcode)
						, info.numOperands
						, info.numValues
						);
				break;
			}

			BX_WARN(size/4 == _instruction.length, "read %d, expected %d, %s"
					, size/4
					, _instruction.length
					, getName(_instruction.opcode)
					);
			while (size/4 != _instruction.length)
			{
				uint32_t tmp;
				size += bx::read(_reader, tmp);
			}
		}

		return size;
	}

	int32_t write(bx::WriterI* _writer, const SpirvInstruction& _instruction)
	{
		int32_t size = 0;
		BX_UNUSED(_writer, _instruction);
		return size;
	}

	int32_t toString(char* _out, int32_t _size, const SpirvInstruction& _instruction)
	{
		int32_t size = 0;
		size += bx::snprintf(&_out[size], bx::uint32_imax(0, _size-size)
					, "%s %d (%d, %d)"
					, getName(_instruction.opcode)
					, _instruction.numOperands
					, _instruction.un.value[0]
					, _instruction.un.value[1]
					);

		return size;
	}

	int32_t read(bx::ReaderSeekerI* _reader, SpirvShader& _shader)
	{
		int32_t size = 0;

		uint32_t len = uint32_t(bx::getSize(_reader) - bx::seek(_reader) );
		_shader.byteCode.resize(len);
		size += bx::read(_reader, _shader.byteCode.data(), len);

		return size;
	}

	int32_t write(bx::WriterI* _writer, const SpirvShader& _shader)
	{
		int32_t size = 0;
		BX_UNUSED(_writer, _shader);
		return size;
	}

#define SPIRV_MAGIC 0x07230203

	int32_t read(bx::ReaderSeekerI* _reader, Spirv& _spirv)
	{
		int32_t size = 0;

		size += bx::read(_reader, _spirv.header);

		if (size != sizeof(Spirv::Header)
		||  _spirv.header.magic != SPIRV_MAGIC
		   )
		{
			// error
			return -size;
		}

		size += read(_reader, _spirv.shader);

		return size;
	}

	int32_t write(bx::WriterSeekerI* _writer, const Spirv& _spirv)
	{
		int32_t size = 0;
		BX_UNUSED(_writer, _spirv);
		return size;
	}

	void parse(const SpirvShader& _src, SpirvParseFn _fn, void* _userData)
	{
		bx::MemoryReader reader(_src.byteCode.data(), uint32_t(_src.byteCode.size() ) );

		for (uint32_t token = 0, numTokens = uint32_t(_src.byteCode.size() / sizeof(uint32_t) ); token < numTokens;)
		{
			SpirvInstruction instruction;
			uint32_t size = read(&reader, instruction);
			BX_CHECK(size/4 == instruction.length, "read %d, expected %d, %s"
					, size/4
					, instruction.length
					, getName(instruction.opcode)
					);
			BX_UNUSED(size);

			_fn(token * sizeof(uint32_t), instruction, _userData);

			token += instruction.length;
		}
	}

} // namespace bgfx
/*
 * Copyright 2011-2015 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include <string.h>
#include <bx/debug.h>
#include <bx/hash.h>
#include <bx/uint32_t.h>
#include <bx/string.h>
#include <bx/readerwriter.h>

#include "config.h"
#include "vertexdecl.h"

namespace bgfx
{
	static const uint8_t s_attribTypeSizeDx9[AttribType::Count][4] =
	{
		{  4,  4,  4,  4 }, // Uint8
		{  4,  4,  4,  4 }, // Uint10
		{  4,  4,  8,  8 }, // Int16
		{  4,  4,  8,  8 }, // Half
		{  4,  8, 12, 16 }, // Float
	};

	static const uint8_t s_attribTypeSizeDx1x[AttribType::Count][4] =
	{
		{  1,  2,  4,  4 }, // Uint8
		{  4,  4,  4,  4 }, // Uint10
		{  2,  4,  8,  8 }, // Int16
		{  2,  4,  8,  8 }, // Half
		{  4,  8, 12, 16 }, // Float
	};

	static const uint8_t s_attribTypeSizeGl[AttribType::Count][4] =
	{
		{  1,  2,  4,  4 }, // Uint8
		{  4,  4,  4,  4 }, // Uint10
		{  2,  4,  6,  8 }, // Int16
		{  2,  4,  6,  8 }, // Half
		{  4,  8, 12, 16 }, // Float
	};

	static const uint8_t (*s_attribTypeSize[])[AttribType::Count][4] =
	{
		&s_attribTypeSizeDx9,  // Null
		&s_attribTypeSizeDx9,  // Direct3D9
		&s_attribTypeSizeDx1x, // Direct3D11
		&s_attribTypeSizeDx1x, // Direct3D12
		&s_attribTypeSizeGl,   // Metal
		&s_attribTypeSizeGl,   // OpenGLES
		&s_attribTypeSizeGl,   // OpenGL
		&s_attribTypeSizeGl,   // Vulkan
		&s_attribTypeSizeDx9,  // Count
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_attribTypeSize) == RendererType::Count+1);

	void initAttribTypeSizeTable(RendererType::Enum _type)
	{
		s_attribTypeSize[0]                   = s_attribTypeSize[_type];
		s_attribTypeSize[RendererType::Count] = s_attribTypeSize[_type];
	}

	void dbgPrintfVargs(const char* _format, va_list _argList)
	{
		char temp[8192];
		char* out = temp;
		int32_t len = bx::vsnprintf(out, sizeof(temp), _format, _argList);
		if ( (int32_t)sizeof(temp) < len)
		{
			out = (char*)alloca(len+1);
			len = bx::vsnprintf(out, len, _format, _argList);
		}
		out[len] = '\0';
		bx::debugOutput(out);
	}

	void dbgPrintf(const char* _format, ...)
	{
		va_list argList;
		va_start(argList, _format);
		dbgPrintfVargs(_format, argList);
		va_end(argList);
	}

	VertexDecl::VertexDecl()
	{
		// BK - struct need to have ctor to qualify as non-POD data.
		// Need this to catch programming errors when serializing struct.
	}

	VertexDecl& VertexDecl::begin(RendererType::Enum _renderer)
	{
		m_hash = _renderer; // use hash to store renderer type while building VertexDecl.
		m_stride = 0;
		memset(m_attributes, 0xff, sizeof(m_attributes) );
		memset(m_offset, 0, sizeof(m_offset) );

		return *this;
	}

	void VertexDecl::end()
	{
		bx::HashMurmur2A murmur;
		murmur.begin();
		murmur.add(m_attributes, sizeof(m_attributes) );
		murmur.add(m_offset, sizeof(m_offset) );
		m_hash = murmur.end();
	}

	VertexDecl& VertexDecl::add(Attrib::Enum _attrib, uint8_t _num, AttribType::Enum _type, bool _normalized, bool _asInt)
	{
		const uint16_t encodedNorm = (_normalized&1)<<7;
		const uint16_t encodedType = (_type&7)<<3;
		const uint16_t encodedNum  = (_num-1)&3;
		const uint16_t encodeAsInt = (_asInt&(!!"\x1\x1\x1\x0\x0"[_type]) )<<8;
		m_attributes[_attrib] = encodedNorm|encodedType|encodedNum|encodeAsInt;

		m_offset[_attrib] = m_stride;
		m_stride += (*s_attribTypeSize[m_hash])[_type][_num-1];

		return *this;
	}

	VertexDecl& VertexDecl::skip(uint8_t _num)
	{
		m_stride += _num;

		return *this;
	}

	void VertexDecl::decode(Attrib::Enum _attrib, uint8_t& _num, AttribType::Enum& _type, bool& _normalized, bool& _asInt) const
	{
		uint16_t val = m_attributes[_attrib];
		_num        = (val&3)+1;
		_type       = AttribType::Enum( (val>>3)&7);
		_normalized = !!(val&(1<<7) );
		_asInt      = !!(val&(1<<8) );
	}

	static const char* s_attrName[] =
	{
		"Attrib::Position",
		"Attrib::Normal",
		"Attrib::Tangent",
		"Attrib::Bitangent",
		"Attrib::Color0",
		"Attrib::Color1",
		"Attrib::Indices",
		"Attrib::Weights",
		"Attrib::TexCoord0",
		"Attrib::TexCoord1",
		"Attrib::TexCoord2",
		"Attrib::TexCoord3",
		"Attrib::TexCoord4",
		"Attrib::TexCoord5",
		"Attrib::TexCoord6",
		"Attrib::TexCoord7",
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_attrName) == Attrib::Count);

	const char* getAttribName(Attrib::Enum _attr)
	{
		return s_attrName[_attr];
	}

	void dump(const VertexDecl& _decl)
	{
		if (BX_ENABLED(BGFX_CONFIG_DEBUG) )
		{
			dbgPrintf("vertexdecl %08x (%08x), stride %d\n"
				, _decl.m_hash
				, bx::hashMurmur2A(_decl.m_attributes)
				, _decl.m_stride
				);

			for (uint32_t attr = 0; attr < Attrib::Count; ++attr)
			{
				if (UINT16_MAX != _decl.m_attributes[attr])
				{
					uint8_t num;
					AttribType::Enum type;
					bool normalized;
					bool asInt;
					_decl.decode(Attrib::Enum(attr), num, type, normalized, asInt);

					dbgPrintf("\tattr %d - %s, num %d, type %d, norm %d, asint %d, offset %d\n"
						, attr
						, getAttribName(Attrib::Enum(attr) )
						, num
						, type
						, normalized
						, asInt
						, _decl.m_offset[attr]
					);
				}
			}
		}
	}

	struct AttribToId
	{
		Attrib::Enum attr;
		uint16_t id;
	};

	static AttribToId s_attribToId[] =
	{
		// NOTICE:
		// Attrib must be in order how it appears in Attrib::Enum! id is
		// unique and should not be changed if new Attribs are added.
		{ Attrib::Position,  0x0001 },
		{ Attrib::Normal,    0x0002 },
		{ Attrib::Tangent,   0x0003 },
		{ Attrib::Bitangent, 0x0004 },
		{ Attrib::Color0,    0x0005 },
		{ Attrib::Color1,    0x0006 },
		{ Attrib::Indices,   0x000e },
		{ Attrib::Weight,    0x000f },
		{ Attrib::TexCoord0, 0x0010 },
		{ Attrib::TexCoord1, 0x0011 },
		{ Attrib::TexCoord2, 0x0012 },
		{ Attrib::TexCoord3, 0x0013 },
		{ Attrib::TexCoord4, 0x0014 },
		{ Attrib::TexCoord5, 0x0015 },
		{ Attrib::TexCoord6, 0x0016 },
		{ Attrib::TexCoord7, 0x0017 },
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_attribToId) == Attrib::Count);

	Attrib::Enum idToAttrib(uint16_t id)
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(s_attribToId); ++ii)
		{
			if (s_attribToId[ii].id == id)
			{
				return s_attribToId[ii].attr;
			}
		}

		return Attrib::Count;
	}

	uint16_t attribToId(Attrib::Enum _attr)
	{
		return s_attribToId[_attr].id;
	}

	struct AttribTypeToId
	{
		AttribType::Enum type;
		uint16_t id;
	};

	static AttribTypeToId s_attribTypeToId[] =
	{
		// NOTICE:
		// AttribType must be in order how it appears in AttribType::Enum!
		// id is unique and should not be changed if new AttribTypes are
		// added.
		{ AttribType::Uint8,  0x0001 },
		{ AttribType::Uint10, 0x0005 },
		{ AttribType::Int16,  0x0002 },
		{ AttribType::Half,   0x0003 },
		{ AttribType::Float,  0x0004 },
	};
	BX_STATIC_ASSERT(BX_COUNTOF(s_attribTypeToId) == AttribType::Count);

	AttribType::Enum idToAttribType(uint16_t id)
	{
		for (uint32_t ii = 0; ii < BX_COUNTOF(s_attribTypeToId); ++ii)
		{
			if (s_attribTypeToId[ii].id == id)
			{
				return s_attribTypeToId[ii].type;
			}
		}

		return AttribType::Count;
	}

	uint16_t attribTypeToId(AttribType::Enum _attr)
	{
		return s_attribTypeToId[_attr].id;
	}

	int32_t write(bx::WriterI* _writer, const VertexDecl& _decl)
	{
		int32_t total = 0;
		uint8_t numAttrs = 0;

		for (uint32_t attr = 0; attr < Attrib::Count; ++attr)
		{
			numAttrs += UINT16_MAX == _decl.m_attributes[attr] ? 0 : 1;
		}

		total += bx::write(_writer, numAttrs);
		total += bx::write(_writer, _decl.m_stride);

		for (uint32_t attr = 0; attr < Attrib::Count; ++attr)
		{
			if (UINT16_MAX != _decl.m_attributes[attr])
			{
				uint8_t num;
				AttribType::Enum type;
				bool normalized;
				bool asInt;
				_decl.decode(Attrib::Enum(attr), num, type, normalized, asInt);
				total += bx::write(_writer, _decl.m_offset[attr]);
				total += bx::write(_writer, s_attribToId[attr].id);
				total += bx::write(_writer, num);
				total += bx::write(_writer, s_attribTypeToId[type].id);
				total += bx::write(_writer, normalized);
				total += bx::write(_writer, asInt);
			}
		}

		return total;
	}

	int32_t read(bx::ReaderI* _reader, VertexDecl& _decl)
	{
		int32_t total = 0;

		uint8_t numAttrs;
		total += bx::read(_reader, numAttrs);

		uint16_t stride;
		total += bx::read(_reader, stride);

		_decl.begin();

		for (uint32_t ii = 0; ii < numAttrs; ++ii)
		{
			uint16_t offset;
			total += bx::read(_reader, offset);

			uint16_t attribId = 0;
			total += bx::read(_reader, attribId);

			uint8_t num;
			total += bx::read(_reader, num);

			uint16_t attribTypeId;
			total += bx::read(_reader, attribTypeId);

			bool normalized;
			total += bx::read(_reader, normalized);

			bool asInt;
			total += bx::read(_reader, asInt);

			Attrib::Enum     attr = idToAttrib(attribId);
			AttribType::Enum type = idToAttribType(attribTypeId);
			if (Attrib::Count     != attr
			&&  AttribType::Count != type)
			{
				_decl.add(attr, num, type, normalized, asInt);
				_decl.m_offset[attr] = offset;
			}
		}

		_decl.end();
		_decl.m_stride = stride;

		return total;
	}

	void vertexPack(const float _input[4], bool _inputNormalized, Attrib::Enum _attr, const VertexDecl& _decl, void* _data, uint32_t _index)
	{
		if (!_decl.has(_attr) )
		{
			return;
		}

		uint32_t stride = _decl.getStride();
		uint8_t* data = (uint8_t*)_data + _index*stride + _decl.getOffset(_attr);

		uint8_t num;
		AttribType::Enum type;
		bool normalized;
		bool asInt;
		_decl.decode(_attr, num, type, normalized, asInt);

		switch (type)
		{
		default:
		case AttribType::Uint8:
			{
				uint8_t* packed = (uint8_t*)data;
				if (_inputNormalized)
				{
					if (asInt)
					{
						switch (num)
						{
						default: *packed++ = uint8_t(*_input++ * 127.0f + 128.0f);
						case 3:  *packed++ = uint8_t(*_input++ * 127.0f + 128.0f);
						case 2:  *packed++ = uint8_t(*_input++ * 127.0f + 128.0f);
						case 1:  *packed++ = uint8_t(*_input++ * 127.0f + 128.0f);
						}
					}
					else
					{
						switch (num)
						{
						default: *packed++ = uint8_t(*_input++ * 255.0f);
						case 3:  *packed++ = uint8_t(*_input++ * 255.0f);
						case 2:  *packed++ = uint8_t(*_input++ * 255.0f);
						case 1:  *packed++ = uint8_t(*_input++ * 255.0f);
						}
					}
				}
				else
				{
					switch (num)
					{
					default: *packed++ = uint8_t(*_input++);
					case 3:  *packed++ = uint8_t(*_input++);
					case 2:  *packed++ = uint8_t(*_input++);
					case 1:  *packed++ = uint8_t(*_input++);
					}
				}
			}
			break;

		case AttribType::Uint10:
			{
				uint32_t packed = 0;
				if (_inputNormalized)
				{
					if (asInt)
					{
						switch (num)
						{
						default:
						case 3:                packed |= uint32_t(*_input++ * 511.0f + 512.0f);
						case 2: packed <<= 10; packed |= uint32_t(*_input++ * 511.0f + 512.0f);
						case 1: packed <<= 10; packed |= uint32_t(*_input++ * 511.0f + 512.0f);
						}
					}
					else
					{
						switch (num)
						{
						default:
						case 3:                packed |= uint32_t(*_input++ * 1023.0f);
						case 2: packed <<= 10; packed |= uint32_t(*_input++ * 1023.0f);
						case 1: packed <<= 10; packed |= uint32_t(*_input++ * 1023.0f);
						}
					}
				}
				else
				{
					switch (num)
					{
					default:
					case 3:                packed |= uint32_t(*_input++);
					case 2: packed <<= 10; packed |= uint32_t(*_input++);
					case 1: packed <<= 10; packed |= uint32_t(*_input++);
					}
				}
				*(uint32_t*)data = packed;
			}
			break;

		case AttribType::Int16:
			{
				int16_t* packed = (int16_t*)data;
				if (_inputNormalized)
				{
					if (asInt)
					{
						switch (num)
						{
						default: *packed++ = int16_t(*_input++ * 32767.0f);
						case 3:  *packed++ = int16_t(*_input++ * 32767.0f);
						case 2:  *packed++ = int16_t(*_input++ * 32767.0f);
						case 1:  *packed++ = int16_t(*_input++ * 32767.0f);
						}
					}
					else
					{
						switch (num)
						{
						default: *packed++ = int16_t(*_input++ * 65535.0f - 32768.0f);
						case 3:  *packed++ = int16_t(*_input++ * 65535.0f - 32768.0f);
						case 2:  *packed++ = int16_t(*_input++ * 65535.0f - 32768.0f);
						case 1:  *packed++ = int16_t(*_input++ * 65535.0f - 32768.0f);
						}
					}
				}
				else
				{
					switch (num)
					{
					default: *packed++ = int16_t(*_input++);
					case 3:  *packed++ = int16_t(*_input++);
					case 2:  *packed++ = int16_t(*_input++);
					case 1:  *packed++ = int16_t(*_input++);
					}
				}
			}
			break;

		case AttribType::Half:
			{
				uint16_t* packed = (uint16_t*)data;
				switch (num)
				{
				default: *packed++ = bx::halfFromFloat(*_input++);
				case 3:  *packed++ = bx::halfFromFloat(*_input++);
				case 2:  *packed++ = bx::halfFromFloat(*_input++);
				case 1:  *packed++ = bx::halfFromFloat(*_input++);
				}
			}
			break;

		case AttribType::Float:
			memcpy(data, _input, num*sizeof(float) );
			break;
		}
	}

	void vertexUnpack(float _output[4], Attrib::Enum _attr, const VertexDecl& _decl, const void* _data, uint32_t _index)
	{
		if (!_decl.has(_attr) )
		{
			memset(_output, 0, 4*sizeof(float) );
			return;
		}

		uint32_t stride = _decl.getStride();
		uint8_t* data = (uint8_t*)_data + _index*stride + _decl.getOffset(_attr);

		uint8_t num;
		AttribType::Enum type;
		bool normalized;
		bool asInt;
		_decl.decode(_attr, num, type, normalized, asInt);

		switch (type)
		{
		default:
		case AttribType::Uint8:
			{
				uint8_t* packed = (uint8_t*)data;
				if (asInt)
				{
					switch (num)
					{
					default: *_output++ = (float(*packed++) - 128.0f)*1.0f/127.0f;
					case 3:  *_output++ = (float(*packed++) - 128.0f)*1.0f/127.0f;
					case 2:  *_output++ = (float(*packed++) - 128.0f)*1.0f/127.0f;
					case 1:  *_output++ = (float(*packed++) - 128.0f)*1.0f/127.0f;
					}
				}
				else
				{
					switch (num)
					{
					default: *_output++ = float(*packed++)*1.0f/255.0f;
					case 3:  *_output++ = float(*packed++)*1.0f/255.0f;
					case 2:  *_output++ = float(*packed++)*1.0f/255.0f;
					case 1:  *_output++ = float(*packed++)*1.0f/255.0f;
					}
				}
			}
			break;

		case AttribType::Uint10:
			{
				uint32_t packed = *(uint32_t*)data;
				if (asInt)
				{
					switch (num)
					{
					default:
					case 3: *_output++ = (float(packed & 0x3ff) - 512.0f)*1.0f/511.0f; packed >>= 10;
					case 2: *_output++ = (float(packed & 0x3ff) - 512.0f)*1.0f/511.0f; packed >>= 10;
					case 1: *_output++ = (float(packed & 0x3ff) - 512.0f)*1.0f/511.0f;
					}
				}
				else
				{
					switch (num)
					{
					default:
					case 3: *_output++ = float(packed & 0x3ff)*1.0f/1023.0f; packed >>= 10;
					case 2: *_output++ = float(packed & 0x3ff)*1.0f/1023.0f; packed >>= 10;
					case 1: *_output++ = float(packed & 0x3ff)*1.0f/1023.0f;
					}
				}
			}
			break;

		case AttribType::Int16:
			{
				int16_t* packed = (int16_t*)data;
				if (asInt)
				{
					switch (num)
					{
					default: *_output++ = float(*packed++)*1.0f/32767.0f;
					case 3:  *_output++ = float(*packed++)*1.0f/32767.0f;
					case 2:  *_output++ = float(*packed++)*1.0f/32767.0f;
					case 1:  *_output++ = float(*packed++)*1.0f/32767.0f;
					}
				}
				else
				{
					switch (num)
					{
					default: *_output++ = (float(*packed++) + 32768.0f)*1.0f/65535.0f;
					case 3:  *_output++ = (float(*packed++) + 32768.0f)*1.0f/65535.0f;
					case 2:  *_output++ = (float(*packed++) + 32768.0f)*1.0f/65535.0f;
					case 1:  *_output++ = (float(*packed++) + 32768.0f)*1.0f/65535.0f;
					}
				}
			}
			break;

		case AttribType::Half:
			{
				uint16_t* packed = (uint16_t*)data;
				switch (num)
				{
				default: *_output++ = bx::halfToFloat(*packed++);
				case 3:  *_output++ = bx::halfToFloat(*packed++);
				case 2:  *_output++ = bx::halfToFloat(*packed++);
				case 1:  *_output++ = bx::halfToFloat(*packed++);
				}
			}
			break;

		case AttribType::Float:
			memcpy(_output, data, num*sizeof(float) );
			_output += num;
			break;
		}

		switch (num)
		{
		case 1: *_output++ = 0.0f;
		case 2: *_output++ = 0.0f;
		case 3: *_output++ = 0.0f;
		default: break;
		}
	}

	void vertexConvert(const VertexDecl& _destDecl, void* _destData, const VertexDecl& _srcDecl, const void* _srcData, uint32_t _num)
	{
		if (_destDecl.m_hash == _srcDecl.m_hash)
		{
			memcpy(_destData, _srcData, _srcDecl.getSize(_num) );
			return;
		}

		struct ConvertOp
		{
			enum Enum
			{
				Set,
				Copy,
				Convert,
			};

			Attrib::Enum attr;
			Enum op;
			uint32_t src;
			uint32_t dest;
			uint32_t size;
		};

		ConvertOp convertOp[Attrib::Count];
		uint32_t numOps = 0;

		for (uint32_t ii = 0; ii < Attrib::Count; ++ii)
		{
			Attrib::Enum attr = (Attrib::Enum)ii;

			if (_destDecl.has(attr) )
			{
				ConvertOp& cop = convertOp[numOps];
				cop.attr = attr;
				cop.dest = _destDecl.getOffset(attr);

				uint8_t num;
				AttribType::Enum type;
				bool normalized;
				bool asInt;
				_destDecl.decode(attr, num, type, normalized, asInt);
				cop.size = (*s_attribTypeSize[0])[type][num-1];

				if (_srcDecl.has(attr) )
				{
					cop.src = _srcDecl.getOffset(attr);
					cop.op = _destDecl.m_attributes[attr] == _srcDecl.m_attributes[attr] ? ConvertOp::Copy : ConvertOp::Convert;
				}
				else
				{
					cop.op = ConvertOp::Set;
				}

				++numOps;
			}
		}

		if (0 < numOps)
		{
			const uint8_t* src = (const uint8_t*)_srcData;
			uint32_t srcStride = _srcDecl.getStride();

			uint8_t* dest = (uint8_t*)_destData;
			uint32_t destStride = _destDecl.getStride();

			float unpacked[4];

			for (uint32_t ii = 0; ii < _num; ++ii)
			{
				for (uint32_t jj = 0; jj < numOps; ++jj)
				{
					const ConvertOp& cop = convertOp[jj];

					switch (cop.op)
					{
					case ConvertOp::Set:
						memset(dest + cop.dest, 0, cop.size);
						break;

					case ConvertOp::Copy:
						memcpy(dest + cop.dest, src + cop.src, cop.size);
						break;

					case ConvertOp::Convert:
						vertexUnpack(unpacked, cop.attr, _srcDecl, src);
						vertexPack(unpacked, true, cop.attr, _destDecl, dest);
						break;
					}
				}

				src += srcStride;
				dest += destStride;
			}
		}
	}

	inline float sqLength(const float _a[3], const float _b[3])
	{
		const float xx = _a[0] - _b[0];
		const float yy = _a[1] - _b[1];
		const float zz = _a[2] - _b[2];
		return xx*xx + yy*yy + zz*zz;
	}

	uint16_t weldVerticesRef(uint16_t* _output, const VertexDecl& _decl, const void* _data, uint16_t _num, float _epsilon)
	{
		// Brute force slow vertex welding...
		const float epsilonSq = _epsilon*_epsilon;

		uint32_t numVertices = 0;
		memset(_output, 0xff, _num*sizeof(uint16_t) );

		for (uint32_t ii = 0; ii < _num; ++ii)
		{
			if (UINT16_MAX != _output[ii])
			{
				continue;
			}

			_output[ii] = (uint16_t)ii;
			++numVertices;

			float pos[4];
			vertexUnpack(pos, Attrib::Position, _decl, _data, ii);

			for (uint32_t jj = 0; jj < _num; ++jj)
			{
				if (UINT16_MAX != _output[jj])
				{
					continue;
				}

				float test[4];
				vertexUnpack(test, Attrib::Position, _decl, _data, jj);

				if (sqLength(test, pos) < epsilonSq)
				{
					_output[jj] = (uint16_t)ii;
				}
			}
		}

		return (uint16_t)numVertices;
	}

	uint16_t weldVertices(uint16_t* _output, const VertexDecl& _decl, const void* _data, uint16_t _num, float _epsilon)
	{
		const uint32_t hashSize = bx::uint32_nextpow2(_num);
		const uint32_t hashMask = hashSize-1;
		const float epsilonSq = _epsilon*_epsilon;

		uint32_t numVertices = 0;

		const uint32_t size = sizeof(uint16_t)*(hashSize + _num);
		uint16_t* hashTable = (uint16_t*)alloca(size);
		memset(hashTable, 0xff, size);

		uint16_t* next = hashTable + hashSize;

		for (uint32_t ii = 0; ii < _num; ++ii)
		{
			float pos[4];
			vertexUnpack(pos, Attrib::Position, _decl, _data, ii);
			uint32_t hashValue = bx::hashMurmur2A(pos, 3*sizeof(float) ) & hashMask;

			uint16_t offset = hashTable[hashValue];
			for (; UINT16_MAX != offset; offset = next[offset])
			{
				float test[4];
				vertexUnpack(test, Attrib::Position, _decl, _data, _output[offset]);

				if (sqLength(test, pos) < epsilonSq)
				{
					_output[ii] = _output[offset];
					break;
				}
			}

			if (UINT16_MAX == offset)
			{
				_output[ii] = (uint16_t)ii;
				next[ii] = hashTable[hashValue];
				hashTable[hashValue] = (uint16_t)ii;
				numVertices++;
			}
		}

		return (uint16_t)numVertices;
	}
} // namespace bgfx
/*
 * Copyright 2011-2014 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

// This shim of sorts is a temporary solution until the go
// tool gets .mm support, or bgfx provides a context API.

#include "bgfx_p.h"

#if BX_PLATFORM_OSX && (BGFX_CONFIG_RENDERER_OPENGLES2|BGFX_CONFIG_RENDERER_OPENGLES3|BGFX_CONFIG_RENDERER_OPENGL)
#	include "renderer_gl.h"
#	include <bx/os.h>

namespace bgfx
{

#	define GL_IMPORT(_optional, _proto, _func, _import) _proto _func
#	include "glimports.h"
	
	struct SwapChainGL
	{
		SwapChainGL(void* _nwh)
		{
			BX_UNUSED(_nwh);
		}

		~SwapChainGL()
		{
		}

		void makeCurrent()
		{
		}

		void swapBuffers()
		{
		}
	};
	
	static void* s_opengl = NULL;

	extern "C"
	{
		struct GlCtx_s
		{
			void* view;
			void* m_context;
		} typedef GlCtx;

		void bgfx_GlContext_create(GlCtx *ctx, void* nsctx, uint32_t _width, uint32_t _height);
		void bgfx_GlContext_destroy(GlCtx *ctx);
		void bgfx_GlContext_resize(GlCtx *ctx, uint32_t _width, uint32_t _height, bool _vsync);
		void bgfx_GlContext_swap(GlCtx *ctx);
	}

	void GlContext::create(uint32_t _width, uint32_t _height)
	{
		BX_UNUSED(_width, _height);

		s_opengl = bx::dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL");
		BX_CHECK(NULL != s_opengl, "OpenGL dynamic library is not found!");

		bgfx_GlContext_create((GlCtx*)this, g_bgfxNSWindow, _width, _height);
		import();
	}

	void GlContext::destroy()
	{
		bgfx_GlContext_destroy((GlCtx*)this);
		bx::dlclose(s_opengl);
	}

	void GlContext::resize(uint32_t _width, uint32_t _height, bool _vsync)
	{
		bgfx_GlContext_resize((GlCtx*)this, _width, _height, _vsync);
	}

	bool GlContext::isSwapChainSupported()
	{
		return false;
	}

	SwapChainGL* GlContext::createSwapChain(void* _nwh)
	{
		return BX_NEW(g_allocator, SwapChainGL)(_nwh);
	}

	void GlContext::destorySwapChain(SwapChainGL* _swapChain)
	{
		BX_DELETE(g_allocator, _swapChain);
	}

	void GlContext::swap(SwapChainGL* _swapChain)
	{
		if (NULL == _swapChain)
		{
			bgfx_GlContext_swap((GlCtx*)this);
		}
		else
		{
			_swapChain->makeCurrent();
			_swapChain->swapBuffers();
		}
	}

	void GlContext::makeCurrent(SwapChainGL* _swapChain)
	{
		if (NULL == _swapChain)
		{
		}
		else
		{
			_swapChain->makeCurrent();
		}
	}

	void GlContext::import()
	{
		BX_TRACE("Import:");
#	define GL_EXTENSION(_optional, _proto, _func, _import) \
				{ \
					if (_func == NULL) \
					{ \
						_func = (_proto)bx::dlsym(s_opengl, #_import); \
						BX_TRACE("%p " #_func " (" #_import ")", _func); \
					} \
					BGFX_FATAL(_optional || NULL != _func, Fatal::UnableToInitialize, "Failed to create OpenGL context. NSGLGetProcAddress(\"%s\")", #_import); \
				}
#	include "glimports.h"
	}

} // namespace bgfx

#endif // BX_PLATFORM_OSX && (BGFX_CONFIG_RENDERER_OPENGLES2|BGFX_CONFIG_RENDERER_OPENGLES3|BGFX_CONFIG_RENDERER_OPENGL)
