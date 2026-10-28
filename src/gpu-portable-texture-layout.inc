// Generated from unchanged native PC layout and common renderer declarations.
GPUAPI_API Float GRenderSettingsMipBias = 0.f;
namespace GpuApi {
CookedTextureMipPlacement CalculateCookedTextureMipChainOffsetAndSize( const TextureDesc& texDesc, Uint32 mip )
	{
// PC cooked data is currently just the packed mips in a linear layout. So just scan through mips and accumulate
		// offset and data size.

		const Uint32 mipCount = texDesc.Internal_CalcTargetNumMips();
		const Uint32 sliceCount = texDesc.CalcTargetSlices();
		RED_ASSERT( mip < mipCount, "Requested mip %u is too large (count=%u)", mip, mipCount );

		Uint32 mipOffset[15];
		RED_ASSERT( mipCount < RED_ARRAY_COUNT( mipOffset ), "Too many mips: %u", mipCount );

		Uint32 totalSize = 0;
		for ( Uint32 mip_i = 0; mip_i < mipCount; ++mip_i )
		{
			mipOffset[mip_i] = totalSize;

			const Uint32 mipWidth	= CalculateTextureMipDimension( texDesc.width, mip_i, texDesc.format );
			const Uint32 mipHeight	= CalculateTextureMipDimension( texDesc.height, mip_i, texDesc.format );
			const Uint32 mipSize	= CalculateUnpaddedTextureSize( mipWidth, mipHeight, texDesc.format ) * sliceCount;

			totalSize += mipSize;
		}

		CookedTextureMipPlacement result;
		result.offset = mipOffset[mip];
		result.size = totalSize - mipOffset[mip];
		return result;

}
}

#ifndef RED_CONFIGURATION_FINAL
#error Original slot-lock selection requires the Final configuration
#endif
namespace GpuApi {
	void LockBuffersSlots( const Uint32 startSlot, Uint32 count )
	{
#ifndef RED_CONFIGURATION_FINAL
		CommandListContext& cl = GetCommandListContext();
		RED_FATAL_ASSERT( startSlot + count <= cl.m_BufferSlotLocks.Size() );
		for ( Uint32 i = 0; i < count; ++i )
		{
			cl.m_BufferSlotLocks.Set( startSlot + i );
		}
#endif
	}
	void UnlockBuffersSlots( const Uint32 startSlot, Uint32 count )
	{
#ifndef RED_CONFIGURATION_FINAL
		CommandListContext& cl = GetCommandListContext();
		RED_FATAL_ASSERT( startSlot + count <= cl.m_BufferSlotLocks.Size() );
		for ( Uint32 i = 0; i < count; ++i )
		{
			cl.m_BufferSlotLocks.Clear( startSlot + i );
		}
#endif
	}
	void LockTexturesSlots( const Uint32 startSlot, Uint32 count )
	{
#ifndef RED_CONFIGURATION_FINAL
		CommandListContext& cl = GetCommandListContext();
		RED_FATAL_ASSERT( startSlot + count <= cl.m_TextureSlotLocks.Size() );
		for( Uint32 i = 0; i < count; ++i )
		{
			cl.m_TextureSlotLocks.Set( startSlot + i );
		}
#endif
	}
	void UnlockTexturesSlots( const Uint32 startSlot, Uint32 count )
	{
#ifndef RED_CONFIGURATION_FINAL
		CommandListContext& cl = GetCommandListContext();
		RED_FATAL_ASSERT( startSlot + count <= cl.m_TextureSlotLocks.Size() );
		for( Uint32 i = 0; i < count; ++i )
		{
			cl.m_TextureSlotLocks.Clear( startSlot + i );
		}
#endif
	}
}
