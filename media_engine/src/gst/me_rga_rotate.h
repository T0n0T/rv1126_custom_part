#ifndef ME_GST_RGA_ROTATE_H
#define ME_GST_RGA_ROTATE_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define ME_TYPE_RGA_ROTATE (me_rga_rotate_get_type())

#define ME_RGA_ROTATE(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj), ME_TYPE_RGA_ROTATE, MeRgaRotate))
#define ME_RGA_ROTATE_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST((klass), ME_TYPE_RGA_ROTATE, MeRgaRotateClass))
#define ME_IS_RGA_ROTATE(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE((obj), ME_TYPE_RGA_ROTATE))
#define ME_RGA_ROTATE_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS((obj), ME_TYPE_RGA_ROTATE, MeRgaRotateClass))

typedef struct _MeRgaRotate MeRgaRotate;
typedef struct _MeRgaRotateClass MeRgaRotateClass;

GType me_rga_rotate_get_type(void);

/* Registers the "rgarotate" element factory (rank NONE, explicit use only).
 * Must be called after gst_init(). */
void me_rga_rotate_register(void);

G_END_DECLS

#endif /* ME_GST_RGA_ROTATE_H */
