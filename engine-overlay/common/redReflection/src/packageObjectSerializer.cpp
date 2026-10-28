/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "packageObjectSerializer.h"
#include "packageSerializer.h"
#include "packageWriteStream.h"
#include "packageReadStream.h"
#include "packageReadWriteStream.h"
#include "package.h"
#include "packageTableOfContent.h"
#include "packageErrorReporter.h"

#include "rttiClass.h"
#include "rttiProperty.h"
#include "rttiArrayTypes.h"
#include "resourceReference.h"
#include "resourceAsyncReference.h"
#include "resourceReferenceScriptToken.h"

// CP2077 web [saves]: package bounds guard helpers (counter lives in packageReadStream.cpp).
#include <atomic>
#include <cstdio>
namespace cpweb_package_guard
{
	// CP2077 web [nomad2]: objects with more than s_packageObjectMaxPropertyCount (128) serialized properties
	// overflowed the fixed StaticArray (stack) in OnSerializeValue/OnRemapValue -> smashed the caller frames
	// (save compile: 'null function' via PackageTableCompiler's toc vtable, or runaway recursion -> RangeError).
	// The arrays are heap DynArrays now; the first few big objects are logged.
	static void ReportBigObject( const char* what, const rtti::IType* type, Uint32 count )
	{
		static std::atomic< Uint32 > s_big{ 0 };
		if( s_big.fetch_add( 1 ) < 12 )
		{
			std::fprintf( stderr, "CP2077 nomad2 save: %s object %s has %u properties (> 128 fixed slots, heap array used)\n",
				what, type ? type->GetName().AsChar() : "?", count );
		}
	}
	void ReportOverrun( const char* what, Uint64 position, Uint64 size, Uint64 bufferSize, const void* base, const void* dest );

	static void ReportObject( const char* what, const rtti::ClassType* objectType, CName propertyName, CName propertyTypeName, Uint32 propertyCount, Uint32 dataOffset )
	{
		static std::atomic< Uint32 > s_count{ 0 };
		if( s_count.fetch_add( 1 ) >= 16 )
			return;
		std::fprintf( stderr, "CP2077 package read guard: %s object=%s properties=%u property=%016llX:%s type=%016llX:%s dataOffset=%u\n",
			what, objectType ? objectType->GetName().ToDebugString() : "?", propertyCount,
			(unsigned long long)propertyName.GetHash(), propertyName.ToDebugString(),
			(unsigned long long)propertyTypeName.GetHash(), propertyTypeName.ToDebugString(), dataOffset );
	}
}

namespace red
{
	const Uint32 s_packageObjectMaxPropertyCount = 128;

	struct PropertyMissingContext
	{
		void * object;
		const rtti::ClassType * objectType;
		CName propertyName;
		CName propertyTypeName;
	};

	struct PropertyMismatchContext
	{
		void * object;
		const rtti::ClassType * objectType;
		const rtti::Property * property;
		const rtti::IType * expectedType;
		CName propertyName;
	};

	void HandlePropertyMissing( const PropertyMissingContext & propertyMissingContext, const PackageTypeSerializer::ReadContext & readContext );
	void HandlePropertyMismatch( const PropertyMismatchContext & propertyMismatchContext, const PackageTypeSerializer::ReadContext & readContext );
	bool CanPropertyMismatchHandled( const PropertyMismatchContext & propertyMismatchContext );
	bool TryAutomaticDataConversion( const PropertyMismatchContext & propertyMismatchContext, const rtti::Variant & data );

	PackageObjectSerializer::PackageObjectSerializer()
	{}

	PackageObjectSerializer::~PackageObjectSerializer()
	{}

	void PackageObjectSerializer::OnWriteValue( const WriteContext & context, const PackageSerializeTypeParameter & param ) const
	{
		const rtti::ClassType * objectType = static_cast< const rtti::ClassType* >( param.type );
			
		const rtti::ClassType::TPropertyList & properties = objectType->GetCachedProperties();
		
		const void* referenceValue = nullptr;

		if( objectType->IsDefaultObjectSerialization() )
		{
			referenceValue = param.referenceValue ? param.referenceValue : objectType->GetDefaultObject();
		}

		struct PropertyValue
		{
			const rtti::Property * property;
			void * data;
			const void * referenceData;
		};

		red::DynArray< PropertyValue > pendingProperties{ red::PoolEngine() }; // [nomad2] was StaticArray<128>
		pendingProperties.Reserve( 64 );

		for( const rtti::Property * property : properties )
		{
			if( !property->IsSerializable() )
				continue;

			if( context.cookingPlatform != PLATFORM_None && !property->IsSerializableInCookedBuilds() )
				continue;
		
			if( ( context.propertyFlags == PackagePropertyType_All ) || ( ( property->GetFlags() & context.propertyFlags ) == context.propertyFlags ) )
			{
				const rtti::IType* propertyType = property->GetType();
				void* propertyData = property->GetOffsetPtr( param.buffer );

				if( referenceValue )
				{
					const void* defaultData = property->GetOffsetPtr( referenceValue );

					if( !propertyType->Compare( defaultData, propertyData, 0 ) )
					{
						pendingProperties.PushBack( {property, propertyData, defaultData} );
					}
				}
				else
				{
					pendingProperties.PushBack( {property, propertyData, nullptr} );
				}
			}
		}

		const Uint64 originPosition = context.stream.GetPosition();

		if( pendingProperties.Size() > s_packageObjectMaxPropertyCount )
		{
			cpweb_package_guard::ReportBigObject( "write", objectType, pendingProperties.Size() );
		}
		Uint16 propertyCount = pendingProperties.Size();
		context.serializer << propertyCount;

		if( propertyCount )
		{	
			const Uint64 startPosition = context.stream.GetPosition();

			red::DynArray< PropertyDescriptor > descriptors{ red::PoolEngine() }; // [nomad2] was StaticArray<128>
			descriptors.Resize( propertyCount );

			const Uint64 startDataPosition = startPosition + descriptors.DataSize();
			context.stream.Seek( startDataPosition );

			for( Uint32 index = 0; index != propertyCount; ++index )
			{
				const PropertyValue & value =  pendingProperties[ index ];

				const rtti::IType * propertyType = value.property->GetType();
				const CName propertyName = value.property->GetName();
				const CName propertyNameType = propertyType->GetName();

				PropertyDescriptor descriptor = 
				{
					context.table.MapName( propertyName ),
					context.table.MapName( propertyNameType ),
					static_cast< Uint32 >( context.stream.GetPosition() - originPosition )
				};

				descriptors[ index ] = descriptor;

				PackageSerializeTypeParameter serializeContext =
				{
					value.data,
					propertyType,
					value.referenceData
				};

				context.serializer.SerializeType( serializeContext );
			}

			red::BlobSpan inplaceDescriptor = context.stream.GetRange( startPosition, startDataPosition );
			red::Memcpy( inplaceDescriptor.Data(), descriptors.Data(), descriptors.DataSize() );
		}
	}

	void PackageObjectSerializer::OnReadValue( const ReadContext & context, const PackageSerializeTypeParameter & param ) const
	{
		const rtti::ClassType * objectType = static_cast< const rtti::ClassType* >( param.type );

		const Uint64 originPosition = context.stream.GetPosition();

		Uint16 propertyCount = 0;
		context.serializer >> propertyCount;

		if( propertyCount )
		{
			rtti::ITypeSystem& rttiSystem = GetRttiSystem();

			// CP2077 web [saves]: the descriptor table is read in place (GetReadCursor), so OnRead's guard does
			// not cover it. Validate the table and every data offset against the buffer (OnRemapValue already
			// checks dataPosition <= bufferSize; the read path did not).
			const Uint64 cpwebBufferSize = context.stream.GetBufferSize();
			const Uint64 cpwebTablePosition = context.stream.GetPosition();
			const Uint64 cpwebTableBytes = Uint64( propertyCount ) * sizeof( PropertyDescriptor );
			if( cpwebTablePosition > cpwebBufferSize || cpwebTableBytes > cpwebBufferSize - cpwebTablePosition )
			{
				cpweb_package_guard::ReportOverrun( "descriptor-table", cpwebTablePosition, cpwebTableBytes, cpwebBufferSize, nullptr, param.buffer );
				cpweb_package_guard::ReportObject( "descriptor-table", objectType, CName(), CName(), propertyCount, 0 );
				context.stream.Seek( cpwebBufferSize );
				return;
			}

			red::ArraySpan< const PropertyDescriptor > descriptors( static_cast< const PropertyDescriptor* >( context.stream.GetReadCursor() ), propertyCount );
			context.stream.Skip( descriptors.SizeInBytes() );

			for( Uint32 decriptorIndex = 0, end = descriptors.Size(); decriptorIndex != end;  ++decriptorIndex )
			{
				const PropertyDescriptor& descriptor = descriptors[decriptorIndex];

				// ctremblay: IMPORTANT - Stream must ALWAYS be in correct position, even if some error occurred. At least in non final configuration.
				// This is needed for Error Handling. System will try to serialize property into a temp storage for user to try to recuperate data.
				const Uint64 dataPosition = originPosition + descriptor.dataOffset;
				if( dataPosition > cpwebBufferSize )
				{
					cpweb_package_guard::ReportOverrun( "property-offset", dataPosition, 0, cpwebBufferSize, nullptr, param.buffer );
					cpweb_package_guard::ReportObject( "property-offset", objectType, context.table.UnmapName( descriptor.nameIndex ), context.table.UnmapName( descriptor.typeNameIndex ), propertyCount, descriptor.dataOffset );
					continue;
				}
				context.stream.Seek( dataPosition );
				
				const CName propertyName = context.table.UnmapName( descriptor.nameIndex );
				const CName propertyTypeName = context.table.UnmapName( descriptor.typeNameIndex ); 
				if( !propertyName.Empty() && !propertyTypeName.Empty() )
				{
					const rtti::IType * expectedType = rttiSystem.FindType( propertyTypeName );
					const rtti::Property * property = objectType->FindProperty( propertyName );
				
					if( expectedType && property && property->IsSerializable() )
					{
						const rtti::IType * propertyType = property->GetType();
						
						if( expectedType == propertyType || rtti::AreTypesSerializationCompatible( expectedType, propertyType ) )
						{
							void * propertyData = property->GetOffsetPtr( param.buffer );

							PackageSerializeTypeParameter propertyParam =
							{
								propertyData,
								propertyType,
								nullptr
							};

							context.serializer.SerializeType( propertyParam );
						}
						else
						{
//#ifndef RED_CONFIGURATION_FINAL
							const PropertyMismatchContext propertyMismatchContext =
							{
								param.buffer,
								objectType,
								property,
								expectedType,
								propertyName,
							};
							
							RED_LOG_ERROR( "Property type mismatch! propertyName:[%016llX,%hs] propertyTypeName:[%016llX,%hs]",
								propertyName.GetHash(), propertyName.ToDebugString(),
								propertyTypeName.GetHash(), propertyTypeName.ToDebugString() );

							HandlePropertyMismatch( propertyMismatchContext, context );
//#endif
						}
					}
					else
					{ 
//#ifndef RED_CONFIGURATION_FINAL
						// Property could not be found. Report Error, skip serialization and move to next property! 
						const PropertyMissingContext propertyMissingContext = 
						{  
							param.buffer,
							objectType,
							propertyName,
							propertyTypeName
						};

						RED_LOG_ERROR( "Property type missing! propertyName:[%016llX,%hs] propertyTypeName:[%016llX,%hs]",
							propertyName.GetHash(), propertyName.ToDebugString(),
							propertyTypeName.GetHash(), propertyTypeName.ToDebugString() );

						cpweb_package_guard::ReportObject( "property-missing", objectType, propertyName, propertyTypeName, propertyCount, descriptor.dataOffset );
						HandlePropertyMissing( propertyMissingContext, context );
//#endif
					}
				}
				else
				{
					/* ctremblay: Corrupted Data! TODO - report error! */
					RED_LOG_ERROR( "Corrupted Data! propertyName:[%016llX,%hs] propertyTypeName:[%016llX,%hs]",
						propertyName.GetHash(), propertyName.ToDebugString(),
						propertyTypeName.GetHash(), propertyTypeName.ToDebugString() );
				}
			}
		}	
	}

	void PackageObjectSerializer::OnRemapValue( const RemapContext& context, const PackageSerializeTypeParameter & param ) const
	{
		// ctremblay: Absolutely dreadful. 
		// Serializer vs Stream vs TypeSerializer is kinda flawed for remapping mostly because of Differential Serialization.
		// Code got worst with moving to 32bits object index...
		
		const Uint64 originInputPosition = context.inputStream.GetPosition();
		const Uint64 originOutputPosition = context.outputStream.GetPosition();

		Uint16 propertyCount = 0;
		context.serializer >> propertyCount;

		if( propertyCount )
		{
			rtti::ITypeSystem& rttiSystem = GetRttiSystem();
			const rtti::ClassType * objectType = static_cast< const rtti::ClassType* >( param.type );
		
			if( propertyCount > s_packageObjectMaxPropertyCount )
			{
				cpweb_package_guard::ReportBigObject( "remap", objectType, propertyCount );
			}
			{
				const Uint64 cpwebPos = context.inputStream.GetPosition();
				const Uint64 cpwebSize = context.inputStream.GetBufferSize();
				if( cpwebPos > cpwebSize || Uint64( propertyCount ) * sizeof( PropertyDescriptor ) > cpwebSize - cpwebPos )
				{
					cpweb_package_guard::ReportOverrun( "remap-descriptor-table", cpwebPos, Uint64( propertyCount ) * sizeof( PropertyDescriptor ), cpwebSize, nullptr, nullptr );
					cpweb_package_guard::ReportObject( "remap-descriptor-table", objectType, CName(), CName(), propertyCount, 0 );
					return;
				}
			}
			red::ArraySpan< const PropertyDescriptor > inputDescriptors( static_cast< const PropertyDescriptor* >( context.inputStream.GetReadCursor() ), propertyCount );
			
			const Uint64 startPosition = context.outputStream.GetPosition();

			red::DynArray< PropertyDescriptor > descriptors{ red::PoolEngine() }; // [nomad2] was StaticArray<128>
			descriptors.Resize( propertyCount );

			const Uint64 startDataPosition = startPosition + descriptors.DataSize();
			context.outputStream.Seek( startDataPosition );
			
			for( Uint32 propertyIndex = 0; propertyIndex != propertyCount; ++propertyIndex )
			{
				const PropertyDescriptor & inputDescriptor = inputDescriptors[ propertyIndex ];
				PropertyDescriptor & outputDescriptor = descriptors[ propertyIndex ];

				const CName propertyName = context.table.UnmapName( inputDescriptor.nameIndex );
				const CName propertyTypeName = context.table.UnmapName( inputDescriptor.typeNameIndex );
				outputDescriptor.nameIndex = context.table.MapName( propertyName );
				outputDescriptor.typeNameIndex = context.table.MapName( propertyTypeName );
				outputDescriptor.dataOffset = static_cast< Uint32 >( context.outputStream.GetPosition() - originOutputPosition );

				const rtti::IType * propertyType = nullptr;
				const rtti::Property * property = objectType->FindProperty( propertyName );
				
				if( property )
				{
					propertyType = property->GetType();
				}
				else if( context.remapMissingProperty )
				{
					propertyType = rttiSystem.FindType( propertyTypeName );
				}

				if( propertyType )
				{
					const rtti::IType * expectedType = context.table.AcquireType( propertyTypeName );
					if( expectedType )
					{
						if( propertyType != expectedType )
						{
							// ctremblay: cannot remap different type. it would result to crash.
							propertyType = expectedType;
						}

						const Uint64 dataPosition = originInputPosition + inputDescriptor.dataOffset;
						const Uint32 bufferSize = context.inputStream.GetBufferSize();

						if( dataPosition <= bufferSize )
						{
							context.inputStream.Seek( dataPosition );
							PackageSerializeTypeParameter typeParam = { nullptr, propertyType, nullptr };
							context.serializer.SerializeType( typeParam );
						}
						else
						{
							// Something terribly wrong is happening here. I cannot continue remapping.
							// Change from struct to IScriptable can cause this. or data corruption
							RED_LOG_ERROR( "Something terribly wrong is happening here. Cannot continue remapping. propertyName:[%016llX,%hs] propertyTypeName:[%016llX,%hs]",
								propertyName.GetHash(), propertyName.ToDebugString(),
								propertyTypeName.GetHash(), propertyTypeName.ToDebugString() );
							return;
						}
					}
					else
					{
						// ctremblay: Type doesn't exist anymore. I can't convert or remap as I have no idea what the type layout was anymore. Moving on.
						// Should a warning or error by log ? There is literally nothing I can do however ... Except finding in code or script the old type.
						RED_LOG_ERROR( "Type doesn't exist anymore. Can't convert or remap as type layout is unknown. propertyName:[%016llX,%hs] propertyTypeName:[%016llX,%hs]",
							propertyName.GetHash(), propertyName.ToDebugString(),
							propertyTypeName.GetHash(), propertyTypeName.ToDebugString() );
					}
				}
			}

			red::BlobSpan inplaceDescriptor = context.outputStream.GetRange( startPosition, startDataPosition );
			red::Memcpy( inplaceDescriptor.Data(), descriptors.Data(), descriptors.DataSize() );
		}
	}

	void HandlePropertyMissing( const PropertyMissingContext & propertyMissingContext, const PackageTypeSerializer::ReadContext & readContext )
	{	
		rtti::ITypeSystem & typeSystem = GetRttiSystem();
		const rtti::IType * propertyType = typeSystem.FindType( propertyMissingContext.propertyTypeName );

		if( propertyType )
		{
			PackageErrorReporter::PropertyMissingContext error =
			{
				propertyMissingContext.propertyName,
				rtti::Variant( propertyType, nullptr )
			};

			PackageSerializeTypeParameter param =
			{
				error.propertyData.Internal_GetData(),
				propertyType,
				nullptr
			};

			// ctremblay: IMPORTANT - Stream is assumed to be already in correct position! 
			readContext.serializer.SerializeType( param );
			readContext.errorReporter.ReportPropertyMissing(std::move( error ) );
		}
		else
		{
			PackageErrorReporter::PropertyUnknownContext error =
			{
				propertyMissingContext.objectType,
				propertyMissingContext.propertyName,
				propertyMissingContext.propertyTypeName
			};

			RED_LOG_ERROR( "Property type mismatch! propertyName:[%016llX,%hs] propertyTypeName:[%016llX,%hs]",
				propertyMissingContext.propertyName.GetHash(), propertyMissingContext.propertyName.ToDebugString(),
				propertyMissingContext.propertyTypeName.GetHash(), propertyMissingContext.propertyTypeName.ToDebugString() );

			readContext.errorReporter.ReportPropertyUnknown( error );
		}
	}

	void HandlePropertyMismatch( const PropertyMismatchContext & propertyMismatchContext, const PackageTypeSerializer::ReadContext & readContext )
	{
		if( CanPropertyMismatchHandled( propertyMismatchContext ) )
		{
			const rtti::IType * expectedType = propertyMismatchContext.expectedType;

			PackageErrorReporter::PropertyMismatchContext error =
			{
				propertyMismatchContext.property,
				propertyMismatchContext.propertyName,
				rtti::Variant( expectedType, nullptr )
			};

			PackageSerializeTypeParameter param =
			{
				error.propertyData.Internal_GetData(),
				expectedType,
				nullptr
			};

			// ctremblay: IMPORTANT - Stream is assumed to be already in correct position! 
			readContext.serializer.SerializeType( param );

			if( !TryAutomaticDataConversion( propertyMismatchContext, error.propertyData ) )
			{				
				readContext.errorReporter.ReportPropertyMismatch( std::move(error) );
			}
		}
	}

	bool CanPropertyMismatchHandled( const PropertyMismatchContext & propertyMismatchContext ) 
	{
		if( propertyMismatchContext.expectedType->GetType() == RT_Array )
		{
			const rtti::IBaseArrayType*	arrayType = static_cast< const rtti::IBaseArrayType* >( propertyMismatchContext.expectedType ); 		
			const rtti::IType * innerType = arrayType->ArrayGetInnerType();
			if( innerType->GetType() == RT_Class )
			{
				const rtti::ClassType * classType = static_cast< const rtti::ClassType* >( innerType );
				return !classType->IsSerializable();
			}
		}

		return true;
	}

	template< typename SrcType, typename DestType >
	static Bool ConvertIntToInt( const rtti::Variant& value, const rtti::Property* prop, void* data )
	{
		static_assert( std::is_integral< SrcType >::value && std::is_integral< DestType >::value, "Only integer types are supported." );

		if ( value.GetRTTIType() == ::GetTypeObject< SrcType >() && prop->GetType() == ::GetTypeObject< DestType >() )
		{
			DestType* dest = reinterpret_cast<DestType*>( prop->GetOffsetPtr( data ) );
			const SrcType* source = reinterpret_cast<const SrcType*>( value.GetData() );
			if ( *source >= std::numeric_limits<DestType>::min() && *source <= std::numeric_limits<DestType>::max() )
			{
				*dest = static_cast< DestType >( *source );
				return true;
			}
		}
		return false;
	}

	bool TryAutomaticDataConversion( const PropertyMismatchContext & propertyMismatchContext, const rtti::Variant & data )
	{
		// ctremblay: TODO, generic handler.
	
		const rtti::Property * prop = propertyMismatchContext.property;
		const rtti::IType * propertyType = prop->GetType();
		void* objectData = propertyMismatchContext.object;

		if( data.GetRTTIType()->GetType() ==  RT_ResourceReference && propertyType->GetType() == RT_ResourceAsyncReference )
		{
			const res::ResourceReference* src = static_cast< const res::ResourceReference* > (data.GetData() );
			res::ResourceAsyncReference* dst = static_cast< res::ResourceAsyncReference* >( prop->GetOffsetPtr( propertyMismatchContext.object ) );
			
			if( src && src->GetPath().IsValid() )
			{
				*dst = res::ResourceAsyncReference( src->GetPath() );
			}
			else
			{
				*dst = res::ResourceAsyncReference();
			}

			return true;
		}

		else if ( data.GetRTTIType()->GetType() == RT_Fundamental && propertyType->GetType() == RT_Fundamental )
		{
			if ( ConvertIntToInt< Uint8, Uint16 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint8, Uint32 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint8, Uint64 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint16, Uint32 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint16, Uint64 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint32, Uint64 >( data, prop, objectData ) )
				return true;

			else if ( ConvertIntToInt< Uint16, Uint8 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint32, Uint8 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint64, Uint8 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint32, Uint16 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint64, Uint16 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Uint64, Uint32 >( data, prop, objectData ) )
				return true;

			else if ( ConvertIntToInt< Int8, Int16 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int8, Int32 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int8, Int64 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int16, Int32 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int16, Int64 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int32, Int64 >( data, prop, objectData ) )
				return true;

			else if ( ConvertIntToInt< Int16, Int8 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int32, Int8 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int64, Int8 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int32, Int16 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int64, Int16 >( data, prop, objectData ) )
				return true;
			else if ( ConvertIntToInt< Int64, Int32 >( data, prop, objectData ) )
				return true;
		}
		else if ( propertyMismatchContext.property->GetType() == GetTypeObject< red::ResourceReferenceScriptToken >() && propertyMismatchContext.expectedType == GetTypeObject< String >() )
		{
			const red::String* src = static_cast< const red::String* > ( data.GetData() );
			red::ResourceReferenceScriptToken* dst = static_cast< red::ResourceReferenceScriptToken* >( prop->GetOffsetPtr( propertyMismatchContext.object ) );
			if ( src )
			{
				*dst = red::ResourceReferenceScriptToken( res::ResourcePath::Build( *src ) );
			}
			else
			{
				*dst = red::ResourceReferenceScriptToken();
			}
			return true;
		}

		return false;	
	}
}
