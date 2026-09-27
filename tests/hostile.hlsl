// state a game can leave bound when it calls Present, which the ui renderer must not draw through (render_test.cpp)

struct gs_in
{
    float4 pos : SV_POSITION;
};

// a geometry shader that swallows every triangle: bound during the ui's draws, nothing of the ui would appear
[maxvertexcount(3)]
void gs_swallow(triangle gs_in input[3], inout TriangleStream<gs_in> output)
{
}
