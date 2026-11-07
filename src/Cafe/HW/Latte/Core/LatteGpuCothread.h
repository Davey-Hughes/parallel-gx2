#pragma once

// OpenGL reaches the GPU through the frontend's context, which libretro makes
// current on the frontend's thread, during the core's callbacks, and nowhere
// else. So under OpenGL the GPU thread is not a thread: it is a cothread that
// the frontend's thread runs from inside those callbacks (retro_run,
// context_reset, context_destroy, the launch and the stop), and every place
// the GPU thread would wait, it hands the frontend's thread back instead.
//
// Vulkan keeps a thread of its own and none of this applies to it: there,
// Latte_GpuYield is std::this_thread::yield and Latte_RunGpu runs nothing.
//
// Everything here is used by one OS thread - the frontend's, which also runs
// the cothread - except Latte_IsOnGpuCothread, which any thread may ask.

enum class LatteGpuYield
{
	Idle,     // no commands to read
	Wait,     // waiting on memory, a semaphore or a flip - all written elsewhere
	Frame,    // a TV frame is finished
	Parked,   // held at the pause gate
	Finished, // the GPU has left, or there is none to run
};

// Frontend side
bool LatteGpuCothread_Start(void (*entry)()); // on the thread that will run it
bool LatteGpuCothread_Exists(); // started and not yet deleted, finished or not
bool LatteGpuCothread_IsOwnerThread(); // the caller is the thread that runs it
void LatteGpuCothread_Delete(); // frees a cothread that has finished
bool Latte_IsGpuCothread(); // a GPU cothread exists and has not finished
LatteGpuYield Latte_RunGpu(); // run the GPU until it next waits
// Inside a wait for something the GPU has to do: run it once, or, with a GPU
// thread of its own, step aside for it. Never blocks.
void Latte_ServeGpu();

// GPU side
bool Latte_IsOnGpuCothread(); // the caller is the GPU cothread
void Latte_GpuYield(LatteGpuYield why); // wait, the cothread way
[[noreturn]] void LatteGpuCothread_Exit(); // the cothread's end; never returns
