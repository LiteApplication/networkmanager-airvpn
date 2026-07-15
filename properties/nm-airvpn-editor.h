/* networkmanager-airvpn - AirVPN integration with NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NM_AIRVPN_EDITOR_H__
#define __NM_AIRVPN_EDITOR_H__

#define AIRVPN_TYPE_EDITOR            (airvpn_editor_get_type ())
#define AIRVPN_EDITOR(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), AIRVPN_TYPE_EDITOR, AirvpnEditor))
#define AIRVPN_EDITOR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), AIRVPN_TYPE_EDITOR, AirvpnEditorClass))
#define AIRVPN_IS_EDITOR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), AIRVPN_TYPE_EDITOR))
#define AIRVPN_IS_EDITOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), AIRVPN_TYPE_EDITOR))
#define AIRVPN_EDITOR_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), AIRVPN_TYPE_EDITOR, AirvpnEditorClass))

typedef struct _AirvpnEditor AirvpnEditor;
typedef struct _AirvpnEditorClass AirvpnEditorClass;

struct _AirvpnEditor {
	GObject parent;
};

struct _AirvpnEditorClass {
	GObjectClass parent;
};

GType airvpn_editor_get_type (void);

NMVpnEditor *nm_airvpn_editor_new (NMConnection *connection, GError **error);

#endif /* __NM_AIRVPN_EDITOR_H__ */
