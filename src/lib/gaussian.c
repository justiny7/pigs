#include "gaussian.h"
#include "math.h"
#include "lib.h"

void init_gaussian_ptr(GaussianPtr *p, Arena* a, uint32_t size) {
    // align to 16 floats bc size isn't necessarily divisible by 16
    uint32_t align = 16 * sizeof(float);
    p->pos_x = arena_alloc_align(a, size * sizeof(float), align);
    p->pos_y = arena_alloc_align(a, size * sizeof(float), align);
    p->pos_z = arena_alloc_align(a, size * sizeof(float), align);

    for (int i = 0; i < 6; i++) {
        p->cov3d[i] = arena_alloc_align(a, size * sizeof(float), align);
    }
    for (int i = 0; i < 16; i++) {
        p->sh_x[i] = arena_alloc_align(a, size * sizeof(float), align);
        p->sh_y[i] = arena_alloc_align(a, size * sizeof(float), align);
        p->sh_z[i] = arena_alloc_align(a, size * sizeof(float), align);
    }

    p->size = size;
}
void init_projected_gaussian_ptr(ProjectedGaussianPtr *p, Arena* a, uint32_t size) {
    // check sizeof(float) = sizeof(uint32_t) for radius_id
    assert(sizeof(float) == sizeof(uint32_t), "Size of float/uint32_t mismatched");

    // align to 16 floats bc size isn't necessarily divisible by 16
    uint32_t align = 16 * sizeof(float);
    p->screen_x = arena_alloc_align(a, size * sizeof(float), align);
    p->screen_y = arena_alloc_align(a, size * sizeof(float), align);
    p->depth = arena_alloc_align(a, size * sizeof(float), align);
    p->cov2d_inv_x = arena_alloc_align(a, size * sizeof(float), align);
    p->cov2d_inv_y = arena_alloc_align(a, size * sizeof(float), align);
    p->cov2d_inv_z = arena_alloc_align(a, size * sizeof(float), align);
    p->color_r = arena_alloc_align(a, size * sizeof(float), align);
    p->color_g = arena_alloc_align(a, size * sizeof(float), align);
    p->color_b = arena_alloc_align(a, size * sizeof(float), align);
    p->opacity = arena_alloc_align(a, size * sizeof(float), align);
    p->radius_id = arena_alloc_align(a, size * sizeof(float), align);
    p->tile = arena_alloc_align(a, size * sizeof(uint32_t), align);

    p->size = size;
}

Vec3 eval_sh(Vec3 pos, Vec3* sh, Vec3 cam_pos) {
    Vec3 dir = vec3_sub(pos, cam_pos);
    dir = vec3_sdiv(dir, vec3_len(dir));

    float x = dir.x, y = dir.y, z = dir.z;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    
    // degree 0
    Vec3 res = vec3_smul(sh[0], SH_C0);

    // degree 1
    res = vec3_sub(res, vec3_smul(sh[1], SH_C1 * y));
    res = vec3_add(res, vec3_smul(sh[2], SH_C1 * z));
    res = vec3_sub(res, vec3_smul(sh[3], SH_C1 * x));

    // degree 2
    res = vec3_add(res, vec3_smul(sh[4], SH_C2[0] * xy));
    res = vec3_add(res, vec3_smul(sh[5], SH_C2[1] * yz));
    res = vec3_add(res, vec3_smul(sh[6], SH_C2[2] * (2.0f * zz - xx - yy)));
    res = vec3_add(res, vec3_smul(sh[7], SH_C2[3] * xz));
    res = vec3_add(res, vec3_smul(sh[8], SH_C2[4] * (xx - yy)));

    // degree 3
    res = vec3_add(res, vec3_smul(sh[9], SH_C3[0] * y * (3.0f * xx - yy)));
    res = vec3_add(res, vec3_smul(sh[10], SH_C3[1] * xy * z));
    res = vec3_add(res, vec3_smul(sh[11], SH_C3[2] * y * (4.0f * zz - xx - yy)));
    res = vec3_add(res, vec3_smul(sh[12], SH_C3[3] * z * (2.0f * zz - 3.0f * xx - 3.0f * yy)));
    res = vec3_add(res, vec3_smul(sh[13], SH_C3[4] * x * (4.0f * zz - xx - yy)));
    res = vec3_add(res, vec3_smul(sh[14], SH_C3[5] * z * (xx - yy)));
    res = vec3_add(res, vec3_smul(sh[15], SH_C3[6] * x * (xx - 3.0f * yy)));

    res = vec3_sadd(res, 0.5f);
    res = vec3_smax(res, 0.0f);

    return res;
}

Mat3 compute_cov3d(Vec3 scale, Vec4 rot) {
    Mat3 S;
    for (int i = 0; i < 9; S.m[i++] = 0.0f);
    S.m[0] = max(expf(scale.x), 1e-6f);
    S.m[4] = max(expf(scale.y), 1e-6f);
    S.m[8] = max(expf(scale.z), 1e-6f);

    Vec4 q = vec4_sdiv(rot, vec4_len(rot));
    Mat3 R = quat_to_rotmat(q);

    Mat3 M = mat3_mm(R, S);
    return mat3_mm(M, mat3_t(M));
}

Vec3 project_cov2d(Vec3 pos, Mat3 cov3d, Mat4 w2c, float fx, float fy) {
    float tx = w2c.m[0] * pos.x + w2c.m[1] * pos.y + w2c.m[2] * pos.z + w2c.m[3];
    float ty = w2c.m[4] * pos.x + w2c.m[5] * pos.y + w2c.m[6] * pos.z + w2c.m[7];
    float tz = w2c.m[8] * pos.x + w2c.m[9] * pos.y + w2c.m[10] * pos.z + w2c.m[11];

    // assert(tz >= 0, "tz is negative");
    // assert(tz >= 0.01, "tz is small");
    // tz = max(tz, 0.1f);
    float tz2 = tz * tz;

    Mat3 J;
    for (int i = 0; i < 9; i++) J.m[i] = 0.0f;
    J.m[0] = fx / tz;
    J.m[2] = -fx * tx / tz2;
    J.m[4] = fy / tz;
    J.m[5] = -fy * ty / tz2;

    Mat3 W;
    W.m[0] = w2c.m[0]; W.m[1] = w2c.m[1]; W.m[2] = w2c.m[2];
    W.m[3] = w2c.m[4]; W.m[4] = w2c.m[5]; W.m[5] = w2c.m[6];
    W.m[6] = w2c.m[8]; W.m[7] = w2c.m[9]; W.m[8] = w2c.m[10];

    Mat3 T = mat3_mm(J, W);
    Mat3 cov = mat3_mm(mat3_mm(T, cov3d), mat3_t(T));

    Vec3 res = { { cov.m[0], cov.m[1], cov.m[4] } };
    return res;
}

Vec3 compute_cov2d_inverse(Vec3 cov2d) {
    float d = max(cov2d.x * cov2d.z - cov2d.y * cov2d.y, 1e-6f);
    assert(d >= 0, "d is neg");
    Vec3 res = { { cov2d.z / d, -cov2d.y / d, cov2d.x / d } };
    return res;
}

float eval_gaussian_2d(float px, float py, float cx, float cy, Vec3 cov_inv) {
    float dx = px - cx;
    float dy = py - cy;
    float power = -0.5f * (cov_inv.x * dx * dx + 2.0f * cov_inv.y * dx * dy + cov_inv.z * dy * dy);
    return expf(power);
}

void precompute_gaussians(Camera* c, Gaussian* g, ProjectedGaussian* pg, uint32_t num_gaussians) {
    for (uint32_t i = 0; i < num_gaussians; i++) {
        project_point(c, g[i].pos, &pg[i].depth, &pg[i].screen_x, &pg[i].screen_y);

        Mat3 cov3d = compute_cov3d(g[i].scale, g[i].rot);
        Vec3 cov2d = project_cov2d(g[i].pos, cov3d, c->w2c, c->fx, c->fy);

        cov2d.x += 0.3f;
        cov2d.z += 0.3f;

        pg[i].cov2d_inv = compute_cov2d_inverse(cov2d);
        pg[i].color = eval_sh(g[i].pos, g[i].sh, c->pos);
        pg[i].opacity = g[i].opacity;

        float mid = 0.5f * (cov2d.x + cov2d.y);
        float det = max(cov2d.x * cov2d.z - cov2d.y * cov2d.y, 1e-6f);
        float lambda1 = mid + sqrtf(max(0.1f, mid * mid - det));
        float lambda2 = mid - sqrtf(max(0.1f, mid * mid - det));
        float r = 3.5f * sqrtf(max(lambda1, lambda2));

        int r_int = (int) r;
        if (r > r_int) {
            pg[i].radius = r + 1;
        } else {
            pg[i].radius = r;
        }

        // pg[i].radius = 3.0f * sqrtf(max(lambda1, lambda2));
    }
}

