#include "gskglrenderopsprivate.h"

void
ops_finish (RenderOpBuilder *builder)
{
  if (builder->mv_stack)
    g_array_free (builder->mv_stack, TRUE);
}

static inline void
rgba_to_float (const GdkRGBA *c,
               float         *f)
{
  f[0] = c->red;
  f[1] = c->green;
  f[2] = c->blue;
  f[3] = c->alpha;
}

/* Debugging only! */
void
ops_dump_framebuffer (RenderOpBuilder *builder,
                      const char      *filename,
                      int              width,
                      int              height)
{
  RenderOp op;

  op.op = OP_DUMP_FRAMEBUFFER;
  op.dump.filename = g_strdup (filename);
  op.dump.width = width;
  op.dump.height = height;

  g_array_append_val (builder->render_ops, op);
}

float
ops_get_scale (const RenderOpBuilder *builder)
{
  const MatrixStackEntry *head;

  g_assert (builder->mv_stack != NULL);
  g_assert (builder->mv_stack->len >= 1);

  head = &g_array_index (builder->mv_stack, MatrixStackEntry, builder->mv_stack->len - 1);

  /* TODO: Use two separate values */
  return MAX (head->metadata.scale_x,
              head->metadata.scale_y);
}

static void
extract_matrix_metadata (const graphene_matrix_t *m,
                         OpsMatrixMetadata       *md)
{
  graphene_vec3_t col1;
  graphene_vec3_t col2;

  /* Translate */
  md->translate_x = graphene_matrix_get_value (m, 3, 0);
  md->translate_y = graphene_matrix_get_value (m, 3, 1);

  /* Scale */
  graphene_vec3_init (&col1,
                      graphene_matrix_get_value (m, 0, 0),
                      graphene_matrix_get_value (m, 1, 0),
                      graphene_matrix_get_value (m, 2, 0));

  graphene_vec3_init (&col2,
                      graphene_matrix_get_value (m, 0, 1),
                      graphene_matrix_get_value (m, 1, 1),
                      graphene_matrix_get_value (m, 2, 1));

  md->scale_x = graphene_vec3_length (&col1);
  md->scale_y = graphene_vec3_length (&col2);

  /* A simple matrix (in our case) is one that doesn't do anything but scale
   * and/or translate.
   *
   * For orher matrices, we fall back to offscreen drawing.
   */
  md->simple = TRUE;
  {
    static const guchar check_zero[4][4] = {
      { 0, 1, 0, 1 },        /* If any of the values marked as '1' here is non-zero, */
      { 1, 0, 0, 1 },        /* We have to resort to offscreen drawing later on. */
      { 1, 1, 0, 1 },
      { 0, 0, 0, 0 },
    };
    int x, y;

    for (x = 0; x < 4; x ++)
      for (y = 0; y < 4; y ++)
          if (check_zero[y][x] &&
              graphene_matrix_get_value (m, y, x) != 0.0f)
            {
              md->simple = FALSE;
              goto out;
            }
  }

out:
  md->only_translation = (md->simple && md->scale_x == 1 && md->scale_y == 1);
}


void
ops_transform_bounds_modelview (const RenderOpBuilder *builder,
                                const graphene_rect_t *src,
                                graphene_rect_t       *dst)
{
  const float scale = ops_get_scale (builder);
  const MatrixStackEntry *head;

  g_assert (builder->mv_stack != NULL);
  g_assert (builder->mv_stack->len >= 1);

  head = &g_array_index (builder->mv_stack, MatrixStackEntry, builder->mv_stack->len - 1);

  if (head->metadata.only_translation)
    {
      *dst = *src;
      graphene_rect_offset (dst,
                            head->metadata.translate_x,
                            head->metadata.translate_y);
    }
  else
    {
      graphene_matrix_transform_bounds (builder->current_modelview,
                                        src,
                                        dst);
    }

  graphene_rect_offset (dst, builder->dx * scale, builder->dy * scale);
}

gboolean
ops_modelview_is_simple (const RenderOpBuilder *builder)
{
  const MatrixStackEntry *head;

  g_assert (builder->mv_stack != NULL);
  g_assert (builder->mv_stack->len >= 1);

  head = &g_array_index (builder->mv_stack, MatrixStackEntry, builder->mv_stack->len - 1);

  return head->metadata.simple;
}

void
ops_set_program (RenderOpBuilder *builder,
                 const Program   *program)
{
  /* The tricky part about this is that we want to initialize all uniforms of a program
   * to the current value from the builder, but only once. */
  static const GskRoundedRect empty_clip;
  static const graphene_matrix_t empty_matrix;
  static const graphene_rect_t empty_rect;
  RenderOp op;
  ProgramState *program_state;

  if (builder->current_program == program)
    return;

  op.op = OP_CHANGE_PROGRAM;
  op.program = program;
  g_array_append_val (builder->render_ops, op);
  builder->current_program = program;

  program_state = &builder->program_state[program->index];

  /* If the projection is not yet set for this program, we use the current one. */
  if (memcmp (&empty_matrix, &program_state->projection, sizeof (graphene_matrix_t)) == 0 ||
      memcmp (&builder->current_projection, &program_state->projection, sizeof (graphene_matrix_t)) != 0)
    {
      op.op = OP_CHANGE_PROJECTION;
      op.projection = builder->current_projection;
      g_array_append_val (builder->render_ops, op);
      program_state->projection = builder->current_projection;
    }

  if (memcmp (&empty_matrix, &program_state->modelview, sizeof (graphene_matrix_t)) == 0 ||
      memcmp (builder->current_modelview, &program_state->modelview, sizeof (graphene_matrix_t)) != 0)
    {
      op.op = OP_CHANGE_MODELVIEW;
      op.modelview = *builder->current_modelview;
      g_array_append_val (builder->render_ops, op);
      program_state->modelview = *builder->current_modelview;
    }

  if (memcmp (&empty_rect, &program_state->viewport, sizeof (graphene_rect_t)) == 0 ||
      memcmp (&builder->current_viewport, &program_state->viewport, sizeof (graphene_rect_t)) != 0)
    {
      op.op = OP_CHANGE_VIEWPORT;
      op.viewport = builder->current_viewport;
      g_array_append_val (builder->render_ops, op);
      program_state->viewport = builder->current_viewport;
    }

  if (memcmp (&empty_clip, &program_state->clip, sizeof (GskRoundedRect)) == 0 ||
      memcmp (&builder->current_clip, &program_state->clip, sizeof (GskRoundedRect)) != 0)
    {
      op.op = OP_CHANGE_CLIP;
      op.clip = builder->current_clip;
      g_array_append_val (builder->render_ops, op);
      program_state->clip = builder->current_clip;
    }

  if (program_state->opacity != builder->current_opacity)
    {
      op.op = OP_CHANGE_OPACITY;
      op.opacity = builder->current_opacity;
      g_array_append_val (builder->render_ops, op);
      program_state->opacity = builder->current_opacity;
    }

  builder->current_program_state = &builder->program_state[program->index];
}

GskRoundedRect
ops_set_clip (RenderOpBuilder      *builder,
              const GskRoundedRect *clip)
{
  RenderOp *last_op;
  GskRoundedRect prev_clip;

  if (builder->render_ops->len > 0)
    {
      last_op = &g_array_index (builder->render_ops, RenderOp, builder->render_ops->len - 1);

      if (last_op->op == OP_CHANGE_CLIP)
        {
          last_op->clip = *clip;
        }
      else
        {
          RenderOp op;

          op.op = OP_CHANGE_CLIP;
          op.clip = *clip;
          g_array_append_val (builder->render_ops, op);
        }
    }

  if (builder->current_program != NULL)
    builder->current_program_state->clip = *clip;

  prev_clip = builder->current_clip;
  builder->current_clip = *clip;

  return prev_clip;
}

static void
ops_set_modelview (RenderOpBuilder         *builder,
                   const graphene_matrix_t *modelview)
{
  RenderOp op;

  if (builder->current_program &&
      memcmp (&builder->current_program_state->modelview, modelview,
              sizeof (graphene_matrix_t)) == 0)
    return;

  if (builder->render_ops->len > 0)
    {
      RenderOp *last_op = &g_array_index (builder->render_ops, RenderOp, builder->render_ops->len - 1);
      if (last_op->op == OP_CHANGE_MODELVIEW)
        {
          last_op->modelview = *modelview;
        }
      else
        {
          op.op = OP_CHANGE_MODELVIEW;
          op.modelview = *modelview;
          g_array_append_val (builder->render_ops, op);
        }
    }
  else
    {
      op.op = OP_CHANGE_MODELVIEW;
      op.modelview = *modelview;
      g_array_append_val (builder->render_ops, op);
    }

  if (builder->current_program != NULL)
    builder->current_program_state->modelview = *modelview;
}

void
ops_push_modelview (RenderOpBuilder         *builder,
                    const graphene_matrix_t *mv)
{
  MatrixStackEntry *entry;

  if (G_UNLIKELY (builder->mv_stack == NULL))
      builder->mv_stack = g_array_new (FALSE, TRUE, sizeof (MatrixStackEntry));

  g_assert (builder->mv_stack != NULL);

  g_array_set_size (builder->mv_stack, builder->mv_stack->len + 1);
  entry = &g_array_index (builder->mv_stack, MatrixStackEntry, builder->mv_stack->len - 1);

  entry->matrix = *mv;
  extract_matrix_metadata (mv, &entry->metadata);

  builder->current_modelview = &entry->matrix;
  ops_set_modelview (builder, mv);
}

void
ops_pop_modelview (RenderOpBuilder *builder)
{
  const graphene_matrix_t *m;
  const MatrixStackEntry *head;

  g_assert (builder->mv_stack);
  g_assert (builder->mv_stack->len >= 1);

  builder->mv_stack->len --;
  head = &g_array_index (builder->mv_stack, MatrixStackEntry, builder->mv_stack->len - 1);
  m = &head->matrix;

  if (builder->mv_stack->len >= 1)
    {
      builder->current_modelview = m;
      ops_set_modelview (builder, m);
    }
  else
    {
      builder->current_modelview = NULL;
    }
}

graphene_matrix_t
ops_set_projection (RenderOpBuilder         *builder,
                    const graphene_matrix_t *projection)
{
  RenderOp op;
  graphene_matrix_t prev_mv;

  if (builder->render_ops->len > 0)
    {
      RenderOp *last_op = &g_array_index (builder->render_ops, RenderOp, builder->render_ops->len - 1);
      if (last_op->op == OP_CHANGE_PROJECTION)
        {
          last_op->projection = *projection;
        }
      else
        {
          op.op = OP_CHANGE_PROJECTION;
          op.projection = *projection;
          g_array_append_val (builder->render_ops, op);
        }
    }
  else
    {
      op.op = OP_CHANGE_PROJECTION;
      op.projection = *projection;
      g_array_append_val (builder->render_ops, op);
    }

  if (builder->current_program != NULL)
    builder->current_program_state->projection = *projection;

  prev_mv = builder->current_projection;
  builder->current_projection = *projection;

  return prev_mv;
}

graphene_rect_t
ops_set_viewport (RenderOpBuilder       *builder,
                  const graphene_rect_t *viewport)
{
  RenderOp op;
  graphene_rect_t prev_viewport;

  op.op = OP_CHANGE_VIEWPORT;
  op.viewport = *viewport;
  g_array_append_val (builder->render_ops, op);

  if (builder->current_program != NULL)
    builder->current_program_state->viewport = *viewport;

  prev_viewport = builder->current_viewport;
  builder->current_viewport = *viewport;

  return prev_viewport;
}

void
ops_set_texture (RenderOpBuilder *builder,
                 int              texture_id)
{
  RenderOp op;

  if (builder->current_texture == texture_id)
    return;

  op.op = OP_CHANGE_SOURCE_TEXTURE;
  op.texture_id = texture_id;
  g_array_append_val (builder->render_ops, op);
  builder->current_texture = texture_id;
}

int
ops_set_render_target (RenderOpBuilder *builder,
                       int              render_target_id)
{
  RenderOp op;
  int prev_render_target;

  if (builder->current_render_target == render_target_id)
    return render_target_id;

  prev_render_target = builder->current_render_target;
  op.op = OP_CHANGE_RENDER_TARGET;
  op.render_target_id = render_target_id;
  g_array_append_val (builder->render_ops, op);
  builder->current_render_target = render_target_id;

  return prev_render_target;
}

float
ops_set_opacity (RenderOpBuilder *builder,
                 float            opacity)
{
  RenderOp op;
  float prev_opacity;
  RenderOp *last_op;

  if (builder->current_opacity == opacity)
    return opacity;

  if (builder->render_ops->len > 0)
    {
      last_op = &g_array_index (builder->render_ops, RenderOp, builder->render_ops->len - 1);

      if (last_op->op == OP_CHANGE_OPACITY)
        {
          last_op->opacity = opacity;
        }
      else
        {
          op.op = OP_CHANGE_OPACITY;
          op.opacity = opacity;
          g_array_append_val (builder->render_ops, op);
        }
    }
  else
    {
      op.op = OP_CHANGE_OPACITY;
      op.opacity = opacity;
      g_array_append_val (builder->render_ops, op);
    }

  prev_opacity = builder->current_opacity;
  builder->current_opacity = opacity;

  if (builder->current_program != NULL)
    builder->current_program_state->opacity = opacity;

  return prev_opacity;
}

void
ops_set_color (RenderOpBuilder *builder,
               const GdkRGBA   *color)
{
  RenderOp op;

  if (gdk_rgba_equal (color, &builder->current_program_state->color))
    return;

  builder->current_program_state->color = *color;

  op.op = OP_CHANGE_COLOR;
  op.color = *color;
  g_array_append_val (builder->render_ops, op);
}

void
ops_set_color_matrix (RenderOpBuilder         *builder,
                      const graphene_matrix_t *matrix,
                      const graphene_vec4_t   *offset)
{
  RenderOp op;

  if (memcmp (matrix,
              &builder->current_program_state->color_matrix.matrix,
              sizeof (graphene_matrix_t)) == 0 &&
      memcmp (offset,
              &builder->current_program_state->color_matrix.offset,
              sizeof (graphene_vec4_t)) == 0)
    return;

  builder->current_program_state->color_matrix.matrix = *matrix;
  builder->current_program_state->color_matrix.offset = *offset;

  op.op = OP_CHANGE_COLOR_MATRIX;
  op.color_matrix.matrix = *matrix;
  op.color_matrix.offset = *offset;
  g_array_append_val (builder->render_ops, op);
}

void
ops_set_border (RenderOpBuilder      *builder,
                const float          *widths,
                const GskRoundedRect *outline)
{
  RenderOp op;

  if (memcmp (&builder->current_program_state->border.widths,
              widths, sizeof (float) * 4) == 0 &&
      memcmp (&builder->current_program_state->border.outline,
              outline, sizeof (GskRoundedRect)) == 0)
    return;

  memcpy (&builder->program_state[builder->current_program->index].border.widths,
          widths, sizeof (float) * 4);

  builder->current_program_state->border.outline = *outline;

  op.op = OP_CHANGE_BORDER;
  op.border.widths[0] = widths[0];
  op.border.widths[1] = widths[1];
  op.border.widths[2] = widths[2];
  op.border.widths[3] = widths[3];
  op.border.outline = *outline;
  g_array_append_val (builder->render_ops, op);
}

void
ops_set_border_color (RenderOpBuilder *builder,
                      const GdkRGBA   *color)
{
  RenderOp op;
  op.op = OP_CHANGE_BORDER_COLOR;
  rgba_to_float (color, op.border.color);

  if (memcmp (&op.border.color, &builder->current_program_state->border.color,
              sizeof (float) * 4) == 0)
    return;

  rgba_to_float (color, builder->current_program_state->border.color);

  g_array_append_val (builder->render_ops, op);
}

void
ops_draw (RenderOpBuilder     *builder,
          const GskQuadVertex  vertex_data[GL_N_VERTICES])
{
  RenderOp *last_op;

  last_op = &g_array_index (builder->render_ops, RenderOp, builder->render_ops->len - 1);
  /* If the previous op was a DRAW as well, we didn't change anything between the two calls,
   * so these are just 2 subsequent draw calls. Same VAO, same program etc.
   * And the offsets into the vao are in order as well, so make it one draw call. */
  if (last_op->op == OP_DRAW)
    {
      /* We allow ourselves a little trick here. We still have to add a CHANGE_VAO op for
       * this draw call so we can add our vertex data there, but we want it to be placed before
       * the last draw call, so we reorder those. */
      RenderOp new_draw;
      new_draw.op = OP_DRAW;
      new_draw.draw.vao_offset = last_op->draw.vao_offset;
      new_draw.draw.vao_size = last_op->draw.vao_size + GL_N_VERTICES;

      last_op->op = OP_CHANGE_VAO;
      memcpy (&last_op->vertex_data, vertex_data, sizeof(GskQuadVertex) * GL_N_VERTICES);

      /* Now add the DRAW */
      g_array_append_val (builder->render_ops, new_draw);
    }
  else
    {
      const gsize n_ops = builder->render_ops->len;
      RenderOp *op;
      gsize offset = builder->buffer_size / sizeof (GskQuadVertex);

      /* We will add two render ops here. */
      g_array_set_size (builder->render_ops, n_ops + 2);

      op = &g_array_index (builder->render_ops, RenderOp, n_ops);
      op->op = OP_CHANGE_VAO;
      memcpy (&op->vertex_data, vertex_data, sizeof(GskQuadVertex) * GL_N_VERTICES);

      op = &g_array_index (builder->render_ops, RenderOp, n_ops + 1);
      op->op = OP_DRAW;
      op->draw.vao_offset = offset;
      op->draw.vao_size = GL_N_VERTICES;
    }

  /* We added new vertex data in both cases so increase the buffer size */
  builder->buffer_size += sizeof (GskQuadVertex) * GL_N_VERTICES;
}

void
ops_offset (RenderOpBuilder *builder,
            float            x,
            float            y)
{
  builder->dx += x;
  builder->dy += y;
}

void
ops_add (RenderOpBuilder *builder,
         const RenderOp  *op)
{
  g_array_append_val (builder->render_ops, *op);
}
