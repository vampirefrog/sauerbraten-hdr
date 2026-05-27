// glstub.h: minimal OpenGL type/constant stubs for the STANDALONE (dedicated server)
// build, which has no GL headers. The dedicated server compiles the octree/edit engine
// (octa/world/octaedit) to maintain a real server-side map, but never renders, so the
// GL types here exist only so the shared data-structure headers (octa.h, texture.h)
// parse. No GL functions are linked; all rendering call sites are guarded out.

#ifndef __GLSTUB_H__
#define __GLSTUB_H__
#ifdef STANDALONE

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef void GLvoid;

// canonical GL enum values (only used as type tags by shader-param bookkeeping)
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_FLOAT 0x1406
#define GL_INT 0x1404
#define GL_BOOL 0x8B56
#define GL_FLOAT_VEC2 0x8B50
#define GL_FLOAT_VEC3 0x8B51
#define GL_FLOAT_VEC4 0x8B52
#define GL_INT_VEC2 0x8B53
#define GL_INT_VEC3 0x8B54
#define GL_INT_VEC4 0x8B55
#define GL_BOOL_VEC2 0x8B57
#define GL_BOOL_VEC3 0x8B58
#define GL_BOOL_VEC4 0x8B59
#define GL_FLOAT_MAT2 0x8B5A
#define GL_FLOAT_MAT3 0x8B5B
#define GL_FLOAT_MAT4 0x8B5C
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_TEXTURE_2D 0x0DE1

// GL extension function pointers referenced by inline shader-param methods in texture.h.
// Declared variadic so those bodies parse; defined NULL in serverstubs.cpp. They are
// never actually invoked on the server (no rendering), so the NULLs are never called.
#define GLSTUB_FNPTR(name) extern void (*name)(...);
GLSTUB_FNPTR(glUniform1f_)  GLSTUB_FNPTR(glUniform2f_)  GLSTUB_FNPTR(glUniform3f_)  GLSTUB_FNPTR(glUniform4f_)
GLSTUB_FNPTR(glUniform1i_)  GLSTUB_FNPTR(glUniform2i_)  GLSTUB_FNPTR(glUniform3i_)  GLSTUB_FNPTR(glUniform4i_)
GLSTUB_FNPTR(glUniform1fv_) GLSTUB_FNPTR(glUniform2fv_) GLSTUB_FNPTR(glUniform3fv_) GLSTUB_FNPTR(glUniform4fv_)
GLSTUB_FNPTR(glUniform1iv_) GLSTUB_FNPTR(glUniform2iv_) GLSTUB_FNPTR(glUniform3iv_) GLSTUB_FNPTR(glUniform4iv_)
GLSTUB_FNPTR(glUniformMatrix2fv_) GLSTUB_FNPTR(glUniformMatrix3fv_) GLSTUB_FNPTR(glUniformMatrix4fv_)

#endif
#endif
