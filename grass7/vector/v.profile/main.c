
/****************************************************************
 *
 * MODULE:     v.profile
 *
 * AUTHOR(S):  Maris Nartiss <maris.gis@gmail.com>
 *             with hints from v.out.ascii, v.buffer, v.what 
 *             and other GRASS modules
 *
 * PURPOSE:    Output vector point/line values along sampling line
 *
 * COPYRIGHT:  (C) 2008 by the GRASS Development Team
 *
 *             This program is free software under the
 *             GNU General Public License (>=v2).
 *             Read the file COPYING that comes with GRASS
 *             for details.
 *
 * TODO:       Attach a centroid to buffer with tolerance value;
 *             Ability to have "interrupted" profiling line - with holes,
 *                 that are not counted into whole profile length;
 *             Implement area sampling by printing out boundary crossing place?
 *             There is no way how to get CAT values;
 *             String quoting is unoptimal:
 *                 * What if delimiter equals string quote character?
 *                 * Quotes within strings are not escaped
 *                 * What if user wants to have different quote symbol or no quotes at all?
 *
 ****************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <grass/config.h>
#include <grass/gis.h>
#include <grass/vector.h>
#include <grass/dbmi.h>
#include <grass/glocale.h>

typedef struct
{
    double distance;
    int cat;
    double z;
} Result;

/* Here are stored all profile line matching points */
static Result *resultset;

/* Qsort distance comparison function */
static int compdist(const void *, const void *);

/* Add point to result data set */
void add_point(struct line_cats *, const double,
	       const double, size_t *, const int);

/* Check if point is on profile line (inside buffer) and calculate distance to it */
void proc_point(struct line_pnts *, struct line_pnts *,
		struct line_pnts *, struct line_cats *, size_t *, const int);

/* Process all two line intersection points */
void proc_line(struct line_pnts *, struct line_pnts *,
	       struct line_cats *, size_t *, const int);

int main(int argc, char *argv[])
{
    struct Map_info In, Out, Pro;
    struct line_pnts *Points, *Profil, *Buffer, *Ipoints;
    struct line_cats *Cats;
    struct ilist *Catlist;
    FILE *ascii;

    int i, dp, type, otype, id, ncols, nrows, ncats, col, more, open3d,
	layer, pro_layer, *cats, c, field_index;
    size_t rescount;
    double xval, yval, bufsize;
    const char *mapset, *pro_mapset;
    char sql[200], *fs;

    struct GModule *module;
    struct Option *old_map, *new_map, *coords_opt, *buffer_opt, *delim_opt,
	*dp_opt, *layer_opt, *where_opt, *inline_map, *inline_where,
	*inline_layer, *type_opt, *file_opt;
    struct Flag *no_column_flag, *no_z_flag;

    struct field_info *Fi, *Fpro;
    dbDriver *driver;
    dbHandle handle;
    dbCursor cursor;
    dbTable *table;
    dbColumn *column;
    dbString table_name, dbsql, valstr;

    /* initialize GIS environment */
    G_gisinit(argv[0]);

    /* initialize module */
    module = G_define_module();
    G_add_keyword(_("vector"));
    G_add_keyword(_("profile"));
    G_add_keyword(_("transect"));
    module->description = _("Vector map profiling tool");

    old_map = G_define_standard_option(G_OPT_V_INPUT);
    old_map->guisection = _("Required");

    type_opt = G_define_standard_option(G_OPT_V_TYPE);
    type_opt->options = "point,line";
    type_opt->answer = "point,line";
    type_opt->guisection = _("Selection");

    coords_opt = G_define_option();
    coords_opt->key = "east_north";
    coords_opt->type = TYPE_DOUBLE;
    coords_opt->key_desc = "east,north";
    coords_opt->required = NO;
    coords_opt->multiple = YES;
    coords_opt->label = _("Coordinates for profiling line nodes");
    coords_opt->description = _("Specify profiling line vertexes and nodes");
    coords_opt->guisection = _("Profiling line");

    buffer_opt = G_define_option();
    buffer_opt->key = "buffer";
    buffer_opt->type = TYPE_DOUBLE;
    buffer_opt->required = YES;
    buffer_opt->answer = "10";
    buffer_opt->label = _("Buffer (tolerance) for points in map units");
    buffer_opt->description = _("How far points can be from sampling line");

    file_opt = G_define_option();
    file_opt->key = "output";
    file_opt->type = TYPE_STRING;
    file_opt->required = NO;
    file_opt->multiple = NO;
    file_opt->gisprompt = "new_file,file,output";
    file_opt->answer = "-";
    file_opt->description = _("Path to output text file or - for stdout");
    file_opt->guisection = _("Format");

    delim_opt = G_define_standard_option(G_OPT_F_SEP);
    delim_opt->guisection = _("Format");

    dp_opt = G_define_option();
    dp_opt->key = "dp";
    dp_opt->type = TYPE_INTEGER;
    dp_opt->required = NO;
    dp_opt->options = "0-32";
    dp_opt->answer = "2";
    dp_opt->description = _("Number of significant digits");
    dp_opt->guisection = _("Format");

    where_opt = G_define_standard_option(G_OPT_DB_WHERE);
    where_opt->guisection = _("Selection");

    layer_opt = G_define_standard_option(G_OPT_V_FIELD);
    layer_opt->answer = "1";
    layer_opt->description = _("Use features only from specified layer");
    layer_opt->guisection = _("Selection");

    new_map = G_define_option();
    new_map->key = "map_output";
    new_map->type = TYPE_STRING;
    new_map->key_desc = "name";
    new_map->required = NO;
    new_map->multiple = NO;
    new_map->gisprompt = "new,vector,vector";
    new_map->label = _("Name for profile line and buffer output map");
    new_map->description =
	_("Profile line and buffer around it will be written");
    new_map->guisection = _("Output");

    no_column_flag = G_define_flag();
    no_column_flag->key = 'c';
    no_column_flag->description = _("Do not print column names");
    no_column_flag->guisection = _("Output");

    no_z_flag = G_define_flag();
    no_z_flag->key = 'z';
    no_z_flag->label = _("Do not print 3D vector data (z values)");
    no_z_flag->description = _("Only affects 3D vectors");
    no_z_flag->guisection = _("Output");

    /* Options that allow to input profiling line from vector map */
    inline_map = G_define_option();
    inline_map->key = "profile_map";
    inline_map->type = TYPE_STRING;
    inline_map->key_desc = "name";
    inline_map->required = NO;
    inline_map->multiple = NO;
    inline_map->gisprompt = "old,vector,vector";
    inline_map->label = _("Profiling line map");
    inline_map->description = _("Vector map containing profiling line");
    inline_map->guisection = _("Profiling line");

    inline_where = G_define_option();
    inline_where->key = "profile_where";
    inline_where->type = TYPE_STRING;
    inline_where->key_desc = "sql_query";
    inline_where->required = NO;
    inline_where->multiple = NO;
    inline_where->label = _("WHERE conditions for input profile line map");
    inline_where->description =
	_("Use to select only one line from profiling line map");
    inline_where->guisection = _("Profiling line");

    inline_layer = G_define_option();
    inline_layer->key = "profile_layer";
    inline_layer->type = TYPE_INTEGER;
    inline_layer->required = NO;
    inline_layer->answer = "1";
    inline_layer->description = _("Profiling line map layer");
    inline_layer->guisection = _("Profiling line");

    if (G_parser(argc, argv))
	exit(EXIT_FAILURE);

    /* Start with simple input validation and then move to more complex ones */
    otype = Vect_option_to_types(type_opt);
    layer = atoi(layer_opt->answer);
    pro_layer = atoi(inline_layer->answer);
    if (layer < 1 || pro_layer < 1)
	G_fatal_error(_("Layer 0 not supported"));

    /* The precision of the output */
    if (dp_opt->answer) {
	if (sscanf(dp_opt->answer, "%d", &dp) != 1)
	    G_fatal_error(_("Failed to interpreter 'dp' parameter as an integer"));
    }

    /* Get buffer size */
    bufsize = fabs(atof(buffer_opt->answer));
    if (bufsize < 0)
	G_fatal_error(_("Tolerance value can not be less than 0"));

    /* If new map name is provided, it has to be useable */
    if (new_map->answer != NULL)
	if (Vect_legal_filename(new_map->answer) < 1)
	    G_fatal_error(_("<%s> is not a valid vector map name"),
			  new_map->answer);

    /* inline_where has no use if inline_map has been not provided */
    if (inline_where->answer != NULL && inline_map->answer == NULL)
	G_fatal_error(_("No input profile map name provided, but WHERE conditions for it have been set"));

    /* Currently only one profile input method is supported */
    if (inline_map->answer != NULL && coords_opt->answer != NULL)
	G_fatal_error(_("Profile input coordinates and vector map are provided. "
		       "Please provide only one of them"));

    if (inline_map->answer == NULL && coords_opt->answer == NULL)
	G_fatal_error(_("No profile input coordinates nor vector map are provided. "
		       "Please provide one of them"));

    /* Where to put module output */
    if (file_opt->answer) {
	if (strcmp(file_opt->answer, "-") == 0) {
	    ascii = stdout;
	}
	else {
	    ascii = fopen(file_opt->answer, "w");
	}

	if (ascii == NULL) {
	    G_fatal_error(_("Unable to open file <%s>"), file_opt->answer);
	}
    }
    else {
	ascii = stdout;
    }

    /* Create and initialize struct's where to store points/lines and categories */
    Points = Vect_new_line_struct();
    Profil = Vect_new_line_struct();
    Buffer = Vect_new_line_struct();
    Ipoints = Vect_new_line_struct();
    Cats = Vect_new_cats_struct();

    /* Construct profile line from user supplied points */
    if (inline_map->answer == NULL) {
	for (i = 0; coords_opt->answers[i] != NULL; i += 2) {
	    xval = atof(coords_opt->answers[i]);
	    yval = atof(coords_opt->answers[i + 1]);
	    Vect_append_point(Profil, xval, yval, 0);
	}

	/* Line is built from two coordinates */
	if (i == 2)
	    G_fatal_error(_("At least profile start and end coordinates are required!"));
    }
    else {
	/* Check provided profile map name validity */
	if ((pro_mapset = G_find_vector2(inline_map->answer, "")) == NULL)
	    G_fatal_error(_("Vector map <%s> not found"), inline_map->answer);
    }

    if ((mapset = G_find_vector2(old_map->answer, "")) == NULL)
	G_fatal_error(_("Vector map <%s> not found"), old_map->answer);

    if (Vect_set_open_level(2))
	G_fatal_error(_("Unable to set predetermined vector open level"));

    /* Open existing vector for reading */
    if (1 > Vect_open_old(&In, old_map->answer, mapset))
	G_fatal_error(_("Unable to open vector map <%s>"), old_map->answer);

    /* Process input as 3D only if it's required */
    if (!no_z_flag->answer && Vect_is_3d(&In))
	open3d = WITH_Z;
    else
	open3d = WITHOUT_Z;

    /* the field separator */
    fs = G_option_to_separator(delim_opt);

    /* Let's get vector layers db connections information */
    Fi = Vect_get_field(&In, layer);
    if (!Fi && where_opt->answer != NULL) {
	Vect_close(&In);
	G_fatal_error(_("No database connection defined for map <%s> layer %d, "
		       "but WHERE condition is provided"), old_map->answer,
		      layer);
    }

    /* Get profile line from existing vector map */
    if (inline_map->answer != NULL) {
	if (1 > Vect_open_old(&Pro, inline_map->answer, pro_mapset))
	    G_fatal_error(_("Unable to open vector map <%s>"),
			  inline_map->answer);
	if (inline_where->answer != NULL) {
	    Fpro = Vect_get_field(&Pro, pro_layer);
	    if (!Fpro) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("No database connection defined for map <%s> layer %d, "
			       "but WHERE condition is provided"),
			      inline_map->answer, pro_layer);
	    }
	    /* Prepeare strings for use in db_* calls */
	    db_init_string(&dbsql);
	    db_init_string(&valstr);
	    db_init_string(&table_name);
	    db_init_handle(&handle);
	    G_debug(1,
		    "Field number:%d; Name:<%s>; Driver:<%s>; Database:<%s>; Table:<%s>; Key:<%s>",
		    Fpro->number, Fpro->name, Fpro->driver, Fpro->database,
		    Fpro->table, Fpro->key);

	    /* Prepearing database for use */
	    driver = db_start_driver(Fpro->driver);
	    if (driver == NULL) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Unable to start driver <%s>"), Fpro->driver);
	    }
	    db_set_handle(&handle, Fpro->database, NULL);
	    if (db_open_database(driver, &handle) != DB_OK) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Unable to open database <%s> by driver <%s>"),
			      Fpro->database, driver);
	    }
	    db_set_string(&table_name, Fpro->table);
	    if (db_describe_table(driver, &table_name, &table) != DB_OK) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Unable to describe table <%s>"),
			      Fpro->table);
	    }
	    ncols = db_get_table_number_of_columns(table);

	    ncats =
		db_select_int(driver, Fpro->table, Fpro->key,
			      inline_where->answer, &cats);
	    if (ncats < 1) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("No features match Your query"));
	    }
	    if (ncats > 1) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Your query matches more than one record in input profiling map. "
			       "Currently it's not supported. Enhance WHERE conditions to get only one line."));
	    }
	    if (!(Catlist = Vect_new_list())) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Error while initialising line list"));
	    }
	    /* Get all features matching specified CAT value */
	    Vect_cidx_find_all(&Pro, pro_layer, GV_LINE, cats[0], Catlist);
	    if (Catlist->n_values > 1) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Your query matches more than one record in input profiling map. "
			       "Currently it's not supported. Enhance WHERE conditions to get only one line."));
	    }
	    if (Vect_read_line(&Pro, Profil, NULL, Catlist->value[0]) !=
		GV_LINE) {
		G_fatal_error(_("Error while reading vector feature from profile line map"));
	    }
	}
	else {
	    /* WHERE not provided -> assuming profiling line map contains only one line */
	    c = 0;
	    while ((type = Vect_read_next_line(&Pro, Profil, NULL)) > 0) {
		if (type & GV_LINE)
		    c++;
	    }
	    /* Currently we support only one SINGLE input line */
	    if (c > 1) {
		Vect_close(&In);
		Vect_close(&Pro);
		G_fatal_error(_("Your input profile map contains more than one line. "
			       "Currently it's not supported. Provide WHERE conditions to get only one line."));
	    }
	}
    }

    /* Create a buffer around profile line for point sampling 
       Tolerance is calculated in such way that buffer will have flat end and no cap. */
    Vect_line_buffer(Profil, bufsize, 1 - (bufsize * cos((2 * M_PI) / 2)),
		     Buffer);
    Vect_cat_set(Cats, 1, 1);

    if (new_map->answer != NULL) {
	/* Open new vector for reading/writing */
	if (0 > Vect_open_new(&Out, new_map->answer, WITHOUT_Z)) {
	    Vect_close(&In);
	    G_fatal_error(_("Unable to create vector map <%s>"),
			  new_map->answer);
	}

	/* Write profile line and it's buffer into new vector map */
	Vect_write_line(&Out, GV_LINE, Profil, Cats);
	/* No category for boundary */
	Vect_reset_cats(Cats);
	Vect_write_line(&Out, GV_BOUNDARY, Buffer, Cats);
    }

    rescount = 0;
    resultset = NULL;

    /* If input vector has a database connection... */
    if (Fi != NULL) {
	/* Prepeare strings for use in db_* calls */
	db_init_string(&dbsql);
	db_init_string(&valstr);
	db_init_string(&table_name);
	db_init_handle(&handle);

	G_debug(1,
		"Field number:%d; Name:<%s>; Driver:<%s>; Database:<%s>; Table:<%s>; Key:<%s>",
		Fi->number, Fi->name, Fi->driver, Fi->database, Fi->table,
		Fi->key);

	/* Prepearing database for use */
	driver = db_start_driver(Fi->driver);
	if (driver == NULL) {
	    Vect_close(&In);
	    G_fatal_error(_("Unable to start driver <%s>"), Fi->driver);
	}
	db_set_handle(&handle, Fi->database, NULL);
	if (db_open_database(driver, &handle) != DB_OK) {
	    Vect_close(&In);
	    G_fatal_error(_("Unable to open database <%s> by driver <%s>"),
			  Fi->database, driver);
	}
	db_set_string(&table_name, Fi->table);
	if (db_describe_table(driver, &table_name, &table) != DB_OK) {
	    Vect_close(&In);
	    G_fatal_error(_("Unable to describe table <%s>"), Fi->table);
	}
	ncols = db_get_table_number_of_columns(table);

	/* Create vector feature list for future processing by applying SQL WHERE conditions... */
	if (where_opt->answer != NULL) {
	    ncats =
		db_select_int(driver, Fi->table, Fi->key, where_opt->answer,
			      &cats);
	    if (ncats < 1)
		G_fatal_error(_("No features match Your query"));
	    field_index = Vect_cidx_get_field_index(&In, layer);
	    for (i = 0; i < ncats; i++) {
		c = Vect_cidx_find_next(&In, field_index, cats[i],
					otype, 0, &type, &id);
		/* Crunch over all lines, that match specified CAT */
		while (c >= 0) {
		    c++;
		    if (type & otype) {
			switch (Vect_read_line(&In, Points, Cats, id)) {
			case GV_POINT:
			    proc_point(Points, Profil, Buffer, Cats,
				       &rescount, open3d);
			    break;
			case GV_LINE:
			    Vect_reset_line(Ipoints);
			    if (Vect_line_get_intersections
				(Profil, Points, Ipoints, open3d) > 0)
				proc_line(Ipoints, Profil, Cats, &rescount,
					  open3d);
			    break;
			}
		    }
		    else
			G_fatal_error
			    ("Error in Vect_cidx_find_next function! Report a bug.");
		    c = Vect_cidx_find_next(&In, field_index, cats[i], otype,
					    c, &type, &id);
		}
	    }
	}
    }

    /* Process all lines IF no database exists or WHERE was not provided.
       Read in single line and get it's type */
    if (Fi == NULL || (where_opt->answer == NULL && Fi != NULL)) {
	while ((type = Vect_read_next_line(&In, Points, Cats)) > 0) {
	    if (type & GV_POINT)
		proc_point(Points, Profil, Buffer, Cats, &rescount, open3d);
	    if (type & GV_LINE) {
		Vect_reset_line(Ipoints);
		if (Vect_line_get_intersections
		    (Profil, Points, Ipoints, open3d) > 0)
		    proc_line(Ipoints, Profil, Cats, &rescount, open3d);
	    }
	}
    }
    /* We don't need input vector anymore */
    Vect_close(&In);
    G_debug(1, "There are %d features matching profile line", rescount);
    /* Sort results by distance, cat */
    qsort(&resultset[0], rescount, sizeof(Result), compdist);

    /* Print out column names */
    if (!no_column_flag->answer) {
	fprintf(ascii, "Number%sDistance", fs);
	if (open3d == WITH_Z)
	    fprintf(ascii, "%sZ", fs);
	if (Fi != NULL) {
	    for (col = 0; col < ncols; col++) {
		column = db_get_table_column(table, col);
		fprintf(ascii, "%s%s", fs, db_get_column_name(column));
	    }
	}
	fprintf(ascii, "\n");
    }

    /* Print out result */
    for (i = 0; i < rescount; i++) {
	fprintf(ascii, "%d%s%.*f", i + 1, fs, dp, resultset[i].distance);
	if (open3d == WITH_Z)
	    fprintf(ascii, "%s%.*f", fs, dp, resultset[i].z);
	if (Fi != NULL) {
	    sprintf(sql, "select * from %s where %s=%d", Fi->table, Fi->key,
		    resultset[i].cat);
	    G_debug(1, "SQL: \"%s\"", sql);
	    db_set_string(&dbsql, sql);
	    if (db_open_select_cursor(driver, &dbsql, &cursor, DB_SEQUENTIAL)
		!= DB_OK)
		G_warning(_("Unabale to get attribute data for cat %d"),
			  resultset[i].cat);
	    else {
		nrows = db_get_num_rows(&cursor);
		G_debug(1, "Result count: %d", nrows);
		table = db_get_cursor_table(&cursor);

		if (nrows > 0) {
		    if (db_fetch(&cursor, DB_NEXT, &more) != DB_OK) {
			G_warning(_("Error while retreiving database record for cat %d"),
				  resultset[i].cat);
		    }
		    else {
			for (col = 0; col < ncols; col++) {
			    /* Column description retreiving is fast, as they live in provided table structure */
			    column = db_get_table_column(table, col);
			    db_convert_column_value_to_string(column,
							      &valstr);
			    type = db_get_column_sqltype(column);

			    /* Those values should be quoted */
			    if (type == DB_SQL_TYPE_CHARACTER ||
				type == DB_SQL_TYPE_DATE ||
				type == DB_SQL_TYPE_TIME ||
				type == DB_SQL_TYPE_TIMESTAMP ||
				type == DB_SQL_TYPE_INTERVAL ||
				type == DB_SQL_TYPE_TEXT ||
				type == DB_SQL_TYPE_SERIAL)
				fprintf(ascii, "%s\"%s\"", fs,
					db_get_string(&valstr));
			    else
				fprintf(ascii, "%s%s", fs,
					db_get_string(&valstr));
			}
		    }
		}
		else {
		    for (col = 0; col < ncols; col++) {
			fprintf(ascii, "%s", fs);
		    }
		}
	    }
	    db_close_cursor(&cursor);
	}
	/* Terminate attribute data line and flush data to provided output (file/stdout) */
	fprintf(ascii, "\n");
	if (fflush(ascii))
            G_fatal_error(_("Can not write data portion to provided output"));
    }

    /* Build topology for vector map and close them */
    if (new_map->answer != NULL) {
	Vect_build(&Out);
	Vect_close(&Out);
    }

    if (Fi != NULL)
	db_close_database_shutdown_driver(driver);

    if (ascii != NULL)
	fclose(ascii);

    /* Don't forget to report to caller sucessfull end of data processing :) */
    exit(EXIT_SUCCESS);
}

/* Qsort distance comparison function */
static int compdist(const void *d1, const void *d2)
{
    Result *r1 = (Result *) d1, *r2 = (Result *) d2;

    G_debug(5, "Comparing %f with %f", r1->distance, r2->distance);

    if (r1->distance == r2->distance) {
	if (r1->cat > r2->cat)
	    return 1;
	else
	    return -1;
    }
    if (r1->distance > r2->distance)
	return 1;
    else
	return -1;
}

/* Add point to result data set */
void add_point(struct line_cats *Cats, const double dist,
	       const double z, size_t *rescount, const int open3d)
{
    Result *tmp;
    
    tmp =
	(Result *) G_realloc(resultset, sizeof(Result) * (*rescount + 1));
    /* Don't leak memory if realloc fails */
    if (!tmp) {
        G_free(resultset);
        G_fatal_error(_("Out of memory"));
    }
    else
        resultset = tmp;
    resultset[*rescount].distance = dist;
    Vect_cat_get(Cats, 1, &resultset[*rescount].cat);
    if (open3d == WITH_Z)
	resultset[*rescount].z = z;
    (*rescount)++;
    G_debug(3, "Distance of point %d is %f", *rescount,
	    resultset[*rescount - 1].distance);
}

/* Check if point is on profile line (inside buffer) and calculate distance to it */
void proc_point(struct line_pnts *Points, struct line_pnts *Profil,
		struct line_pnts *Buffer, struct line_cats *Cats,
		size_t *rescount, const int open3d)
{
    double dist;

    if (Vect_point_in_poly(*Points->x, *Points->y, Buffer) > 0) {
	Vect_line_distance(Profil, *Points->x, *Points->y, *Points->z,
			   open3d, NULL, NULL, NULL, NULL, NULL, &dist);
	add_point(Cats, dist, *Points->z, rescount, open3d);
    }
}

/* Process all line intersection points */
void proc_line(struct line_pnts *Ipoints, struct line_pnts *Profil,
	       struct line_cats *Cats, size_t *rescount, const int open3d)
{
    int i;
    double dist;

    /* Add all line and profile intersection points to resultset */
    for (i = 0; i < Ipoints->n_points; i++) {
	Vect_line_distance(Profil, Ipoints->x[i], Ipoints->y[i],
			   Ipoints->z[i], open3d, NULL, NULL, NULL, NULL,
			   NULL, &dist);
	add_point(Cats, dist, Ipoints->z[i], rescount, open3d);
    }
}