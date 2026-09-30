// Simulated map lights (radiant/simlights.cpp): vertex stage.
//
// Runs on the fixed-function vertex arrays the textured camera pass already
// supplies, so nothing about how brushes, patches and models are submitted
// changes. The surface is lit in world space; q3map2's light entities are
// authored there, and the per-object transform (func_ entities, models) has
// already been folded into gl_ModelViewMatrix.

uniform mat4 u_view_inverse; // camera modelview, inverted: eye space -> world

varying vec3 var_world_pos;
varying vec3 var_world_normal;
varying float var_eye_z; // distance in front of the camera, for the fragment stage's light cluster

void main()
{
	vec4 eye = gl_ModelViewMatrix * gl_Vertex;

	var_eye_z = -eye.z;
	var_world_pos = ( u_view_inverse * eye ).xyz;
	// gl_NormalMatrix undoes the object's scale, the inverse view undoes the camera
	var_world_normal = mat3( u_view_inverse ) * ( gl_NormalMatrix * gl_Normal );

	gl_TexCoord[0] = gl_MultiTexCoord0;
	gl_FrontColor = gl_Color; // alpha carries the shader's translucency
	gl_Position = ftransform();
}
