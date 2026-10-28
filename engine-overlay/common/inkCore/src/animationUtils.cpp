/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "animationUtils.h"
#include "animationDataStructs.h"
#include "interpolationFunctions.h"
#include "interpolationModes.h"

namespace ink
{
	namespace anim
	{
		template<Float( *function )( Float )>
		Float CalculateValueAspectWithMode( InterpolationMode mode, Float timeAspect )
		{
			switch( mode )
			{
			case InterpolationMode::EasyIn:
			{
				return EasyIn<function>( timeAspect );
			}
			break;

			case InterpolationMode::EasyOut:
			{
				return EasyOut<function>( timeAspect );
			}
			break;

			case InterpolationMode::EasyInOut:
			{
				return EasyInOut<function>( timeAspect );
			}
			break;

			case InterpolationMode::EasyOutIn:
			{
				return EasyOutIn<function>( timeAspect );
			}
			break;

			default:
			{
				RED_ASSERT( false, "Incorrect interpolation mode in ink anim system" );
				return 0.0f;
			}
			break;
			}

			RED_ASSERT( false, "Incorrect interpolation mode in ink anim system" );
			return 0.0f;
		}

		Float CalculateInterpolatorValueAspect( const InterpolationType & type, const InterpolationMode & mode, const Float & timeAspect )
		{
			RED_ASSERT( timeAspect >= 0.0f && timeAspect <= 1.0f, "" );

			switch( type )
			{
			case InterpolationType::Linear:
			{
				return CalculateValueAspectWithMode<Linear>( mode, timeAspect );
			}
			break;
			case InterpolationType::Quadratic:
			{
				return CalculateValueAspectWithMode<Quadratic>( mode, timeAspect );
			}
			break;
			case InterpolationType::Qubic:
			{
				return CalculateValueAspectWithMode<Qubic>( mode, timeAspect );
			}
			break;
			case InterpolationType::Quartic:
			{
				return CalculateValueAspectWithMode<Quartic>( mode, timeAspect );
			}
			break;
			case InterpolationType::Quintic:
			{
				return CalculateValueAspectWithMode<Quintic>( mode, timeAspect );
			}
			break;
			case InterpolationType::Sinusoidal:
			{
				return CalculateValueAspectWithMode<Sinusoidal>( mode, timeAspect );
			}
			break;
			case InterpolationType::Exponential:
			{
				return CalculateValueAspectWithMode<Exponential>( mode, timeAspect );
			}
			break;
			case InterpolationType::Elastic:
			{
				return CalculateValueAspectWithMode<Elastic>( mode, timeAspect );
			}
			break;
			case InterpolationType::Circular:
			{
				return CalculateValueAspectWithMode<Circular>( mode, timeAspect );
			}
			break;
			case InterpolationType::Back:
			{
				return CalculateValueAspectWithMode<Back>( mode, timeAspect );
			}
			break;
			default:
			{
				RED_ASSERT( false, "Incorrect interpolation function in ink anim system" );
				return 0.0f;
			}
			break;
			}

			RED_ASSERT( false, "Incorrect interpolation function in ink anim system" );
			return 0.0f;
		}

		Float CalculateInterpolatorTimeAspect( const InterpolationType & type, const InterpolationMode & mode, const Float & valueAspect )
		{
			RED_ASSERT( valueAspect >= 0.0f && valueAspect <= 1.0f, "" );

			switch( type )
			{
				case InterpolationType::Linear:
				{
					return CalculateValueAspectWithMode<InvLinear>( mode, valueAspect );
				}
				break;
				case InterpolationType::Quadratic:
				{
					return CalculateValueAspectWithMode<InvQuadratic>( mode, valueAspect );
				}
				break;
				case InterpolationType::Qubic:
				{
					return CalculateValueAspectWithMode<InvQubic>( mode, valueAspect );
				}
				break;
				case InterpolationType::Quartic:
				{
					return CalculateValueAspectWithMode<InvQuartic>( mode, valueAspect );
				}
				break;
				case InterpolationType::Quintic:
				{
					return CalculateValueAspectWithMode<InvQuintic>( mode, valueAspect );
				}
				break;
				case InterpolationType::Sinusoidal:
				{
					return CalculateValueAspectWithMode<InvSinusoidal>( mode, valueAspect );
				}
				break;
				case InterpolationType::Exponential:
				{
					return CalculateValueAspectWithMode<InvExponential>( mode, valueAspect );
				}
				break;
				case InterpolationType::Elastic:
				{
					return CalculateValueAspectWithMode<InvElastic>( mode, valueAspect );
				}
				break;
				case InterpolationType::Circular:
				{
					return CalculateValueAspectWithMode<InvCircular>( mode, valueAspect );
				}
				break;
				case InterpolationType::Back:
				{
					return CalculateValueAspectWithMode<InvBack>( mode, valueAspect );
				}
				break;
				default:
				{
					RED_ASSERT( false, "Incorrect interpolation function in ink anim system" );
					return 0.0f;
				}
				break;
			}

			RED_ASSERT( false, "Incorrect interpolation function in ink anim system" );
			return 0.0f;
		}
	
	}
}