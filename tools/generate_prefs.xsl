<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
	<xsl:output method="text" omit-xml-declaration="yes" indent="no" />
	<xsl:strip-space elements="*"/>
	<xsl:variable name="lowercase" select="'abcdefghijklmnopqrstuvwxyz'" />
	<xsl:variable name="uppercase" select="'ABCDEFGHIJKLMNOPQRSTUVWXYZ'" />
	<!-- The start of the gui generating functions -->
	<xsl:variable name="tab_start"> (GtkWidget *dialog, GtkWidget *tab, void (*hardcoded_part)(GtkWidget *vbox1, GtkWidget *vbox2))
{
  GtkWidget *widget, *label, *labelev, *viewport;
  GtkRequisition size;
  GtkWidget *hbox = gtk_hbox_new(5, FALSE);
  GtkWidget *vbox1 = gtk_vbox_new(5, TRUE);
  GtkWidget *vbox2 = gtk_vbox_new(5, TRUE);
  char tooltip[1024];
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, FALSE, FALSE, 0);
  GtkWidget *alignment = gtk_alignment_new(0.5, 0.0, 1.0, 0.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 20, 20, 20, 20);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  viewport = gtk_viewport_new(NULL, NULL);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE); // doesn't seem to work from gtkrc
  gtk_container_add(GTK_CONTAINER(alignment), scroll);
  gtk_container_add(GTK_CONTAINER(scroll), viewport);
  gtk_container_add(GTK_CONTAINER(viewport), hbox);
</xsl:variable>

  <xsl:variable name="tab_end">
  if(hardcoded_part)
    (*hardcoded_part)(vbox1, vbox2);

  gtk_widget_show_all(tab);

  gtk_widget_size_request(viewport, &amp;size);
  gtk_widget_set_size_request(scroll, size.width, size.height);
}
</xsl:variable>

	<xsl:param name="HAVE_OPENCL">1</xsl:param>

<!-- The basic structure of the generated file -->

<xsl:template match="/">
	<xsl:text><![CDATA[/** generated file, do not edit! */
#ifndef DT_PREFERENCES_H
#define DT_PREFERENCES_H

#include <gtk/gtk.h>
#include "control/conf.h"

]]></xsl:text>

	<!-- reset callbacks -->

	<xsl:for-each select="./dtconfiglist/dtconfig[@prefs]">
		<xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
			<xsl:text>static gboolean&#xA;reset_widget_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkWidget *label, GdkEventButton *event, GtkWidget *widget)&#xA;{&#xA;  if(event->type == GDK_2BUTTON_PRESS)&#xA;  {&#xA;</xsl:text>
			<xsl:apply-templates select="." mode="reset"/>
			<xsl:text>&#xA;    return TRUE;&#xA;  }&#xA;  return FALSE;&#xA;}&#xA;&#xA;</xsl:text>
		</xsl:if>
	</xsl:for-each>

	<!-- change callbacks -->

	<xsl:for-each select="./dtconfiglist/dtconfig[@prefs]">
		<xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
			<xsl:text>static void&#xA;preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkWidget *widget, gpointer user_data)&#xA;{&#xA;</xsl:text>
			<xsl:apply-templates select="." mode="change"/>
			<xsl:text>&#xA;}&#xA;&#xA;</xsl:text>
		</xsl:if>
	</xsl:for-each>

	<!-- response callbacks (on dialog close) -->

	<xsl:for-each select="./dtconfiglist/dtconfig[@prefs]">
		<xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
			<xsl:text>static void&#xA;preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkDialog *dialog, gint response_id, GtkWidget *widget)&#xA;{&#xA;  if(response_id != GTK_RESPONSE_ACCEPT)&#xA;    return;&#xA;</xsl:text>
			<xsl:apply-templates select="." mode="change"/>
			<xsl:text>&#xA;}&#xA;&#xA;</xsl:text>
		</xsl:if>
	</xsl:for-each>

	<!-- preferences tabs -->
	<!-- gui -->

	<xsl:text>&#xA;static void&#xA;init_tab_gui</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_notebook_append_page(GTK_NOTEBOOK(tab), alignment, gtk_label_new(_("GUI options")));&#xA;</xsl:text>

	<xsl:for-each select="./dtconfiglist/dtconfig[@prefs='gui']">
		<xsl:apply-templates select="." mode="tab_block"/>
	</xsl:for-each>
	<xsl:value-of select="$tab_end" />

	<!-- core -->

	<xsl:text>&#xA;static void&#xA;init_tab_core</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_notebook_append_page(GTK_NOTEBOOK(tab), alignment, gtk_label_new(_("core options")));&#xA;</xsl:text>

	<xsl:for-each select="./dtconfiglist/dtconfig[@prefs='core']">
		<xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
			<xsl:apply-templates select="." mode="tab_block"/>
		</xsl:if>
	</xsl:for-each>
  <xsl:value-of select="$tab_end" />

	<!-- closing credits -->
	<xsl:text>&#xA;#endif&#xA;</xsl:text>

</xsl:template>

<!-- The common blocks inside of the tabs -->

<xsl:template match="dtconfig" mode="tab_block">
	<xsl:text>
  {
    label = gtk_label_new(_("</xsl:text><xsl:value-of select="shortdescription"/><xsl:text>"));
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
</xsl:text>
	<xsl:apply-templates select="." mode="tab"/>
	<xsl:text>    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);</xsl:text>
	<xsl:if test="longdescription != ''">
		<xsl:text>&#xA;    g_object_set(widget, "tooltip-text", _("</xsl:text><xsl:value-of select="longdescription"/><xsl:text>"), (char *)NULL);</xsl:text>
	</xsl:if>
	<xsl:if test="@capability">
                <xsl:text>&#xA;    gtk_widget_set_sensitive(labelev, dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"));</xsl:text>
                <xsl:text>&#xA;    gtk_widget_set_sensitive(widget, dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"));</xsl:text>
                <xsl:text>&#xA;    if(!dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"))</xsl:text>
                <xsl:text>&#xA;      g_object_set(widget, "tooltip-text", _("not available on this system"), (char *)NULL);</xsl:text>
	</xsl:if>
	<xsl:text>
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), (gpointer)widget);
  }
</xsl:text>
</xsl:template>

<!-- Rules handling code specific for different types -->

<!-- RESET -->
	<xsl:template match="dtconfig[type='string']" mode="reset">
		<xsl:text>    gtk_entry_set_text(GTK_ENTRY(widget), "</xsl:text><xsl:value-of select="default"/><xsl:text>");</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='int']" mode="reset">
		<xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='int64']" mode="reset">
		<xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='float']" mode="reset">
		<xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='bool']" mode="reset">
		<xsl:text>    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), </xsl:text><xsl:value-of select="translate(default, $lowercase, $uppercase)"/><xsl:text>);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type/enum]" mode="reset">
		<xsl:variable name="default" select="default"/>
		<xsl:for-each select="./type/enum/option">
			<xsl:if test="$default = .">
				<xsl:text>    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), </xsl:text><xsl:value-of select="position()-1"/><xsl:text>);</xsl:text>
			</xsl:if>
		</xsl:for-each>
	</xsl:template>

<!-- CALLBACK -->
	<xsl:template match="dtconfig[type='string']" mode="change">
		<xsl:text>  dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_entry_get_text(GTK_ENTRY(widget)));</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='int']" mode="change">
		<xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>  dt_conf_set_int("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='int64']" mode="change">
		<xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>  dt_conf_set_int64("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='float']" mode="change">
		<xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>  dt_conf_set_float("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='bool']" mode="change">
		<xsl:text>  dt_conf_set_bool("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type/enum]" mode="change">
		<xsl:text>  dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget)));</xsl:text>
	</xsl:template>

<!-- TAB -->
	<xsl:template match="dtconfig[type='string']" mode="tab">
		<xsl:text>    widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>"));
    g_signal_connect(G_OBJECT(widget), "activate", G_CALLBACK(preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "</xsl:text><xsl:value-of select="default"/><xsl:text>");
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='int']" mode="tab">
		<xsl:text>    gint min = 0;&#xA;    gint max = G_MAXINT;&#xA;</xsl:text>
		<xsl:apply-templates select="type" mode="range"/>
		<xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%d'"), (int)(</xsl:text><xsl:value-of select="default"/><xsl:text> * factor));
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='int64']" mode="tab">
		<xsl:text>    gint64 min = 0;&#xA;    gint64 max = G_MAXINT64;&#xA;</xsl:text>
		<xsl:apply-templates select="type" mode="range"/>
		<xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int64("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%"PRId64"'"), (gint64)(</xsl:text><xsl:value-of select="default"/><xsl:text> * factor));
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type='float']" mode="tab">
		<xsl:text>    float min = -1000000000.0f;&#xA;    float max = 1000000000.0f;&#xA;</xsl:text>
		<xsl:apply-templates select="type" mode="range"/>
		<xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
		<xsl:text>    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 0.001f);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_float("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%.03f'"), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
</xsl:text>
	</xsl:template>

  	<xsl:template match="dtconfig[type='bool']" mode="tab">
		<xsl:text>    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("</xsl:text><xsl:value-of select="name"/><xsl:text>"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "</xsl:text><xsl:value-of select="translate(default, $lowercase, $uppercase)"/><xsl:text>");
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
</xsl:text>
	</xsl:template>

	<xsl:template match="dtconfig[type/enum]" mode="tab">
		<xsl:text>    widget = gtk_combo_box_text_new();
    {
      gchar *str = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
      gint pos = -1;
</xsl:text>
		<xsl:for-each select="./type/enum/option">
			<xsl:text>      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), "</xsl:text><xsl:value-of select="."/><xsl:text>");
      if(pos == -1 &amp;&amp; strcmp(str, "</xsl:text><xsl:value-of select="."/><xsl:text>") == 0)
        pos = </xsl:text><xsl:value-of select="position()-1" /><xsl:text>;
</xsl:text>
		</xsl:for-each>
		<xsl:text>      gtk_combo_box_set_active(GTK_COMBO_BOX(widget), pos);
    }
    g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(preferences_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), NULL);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "</xsl:text><xsl:value-of select="default"/><xsl:text>");
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
</xsl:text>
	</xsl:template>

<!-- Grab min/max from input. Is there a better way? -->
	<xsl:template match="type[@min and @max]" mode="range" priority="5">
		<xsl:text>    min = </xsl:text><xsl:value-of select="@min"/><xsl:text>;&#xA;</xsl:text>
		<xsl:text>    max = </xsl:text><xsl:value-of select="@max"/><xsl:text>;&#xA;</xsl:text>
	</xsl:template>

	<xsl:template match="type[@min]" mode="range" priority="3">
		<xsl:text>    min = </xsl:text><xsl:value-of select="@min"/><xsl:text>;&#xA;</xsl:text>
	</xsl:template>

	<xsl:template match="type[@max]" mode="range" priority="3">
		<xsl:text>    max = </xsl:text><xsl:value-of select="@max"/><xsl:text>;&#xA;</xsl:text>
	</xsl:template>

	<xsl:template match="type" mode="range"  priority="1"/>

<!-- Also look for the factor used in the GUI. -->
	<xsl:template match="type[@factor]" mode="factor" priority="3">
		<xsl:text>  float factor = </xsl:text><xsl:value-of select="@factor"/><xsl:text>;&#xA;</xsl:text>
	</xsl:template>

	<xsl:template match="type" mode="factor"  priority="1">
		<xsl:text>  float factor = 1.0f;&#xA;</xsl:text>
	</xsl:template>


</xsl:stylesheet>
