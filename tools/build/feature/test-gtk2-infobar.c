#pragma GCC diagnostic ignored ""
#include <gtk/gtk.h>
#pragma GCC diagnostic error ""

int main(int argc, char *argv[])
{
	gtk_init(&argc, &argv);
	gtk_info_bar_new();

	return 0;
}
