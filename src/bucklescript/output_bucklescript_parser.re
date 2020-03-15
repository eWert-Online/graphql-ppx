open Migrate_parsetree;
open Graphql_ppx_base;
open Result_structure;
open Schema;

open Ast_408;
open Asttypes;
open Parsetree;

open Generator_utils;
open Output_bucklescript_utils;
open Output_bucklescript_types;

let rec generate_poly_type_ref_name = (type_ref: Graphql_ast.type_ref) => {
  switch (type_ref) {
  | Tr_named({item: name}) => name
  | Tr_list({item: type_ref}) =>
    "ListOf_" ++ generate_poly_type_ref_name(type_ref)
  | Tr_non_null_named({item: name}) => "NonNull_" ++ name
  | Tr_non_null_list({item: type_ref}) =>
    "NonNullListOf_" ++ generate_poly_type_ref_name(type_ref)
  };
};

// let type_name_to_words = type_name => {
//   type_name
//   |> Str.global_replace(Str.regexp("\\["), "")
//   |> Str.global_replace(Str.regexp("\\]!"), "_OfNonNullList")
//   |> Str.global_replace(Str.regexp("\\]"), "_OfList")
//   |> Str.global_replace(Str.regexp("!"), "_NonNull");
// };

let type_name_to_words = type_name => {
  let str = ref("");
  type_name
  |> String.iter(
       fun
       | '!' => str := str^ ++ "_NonNull"
       | ']' => str := str^ ++ "_OfList"
       | c => str := str^ ++ String.make(1, c),
     );
  str^;
};

let get_variable_definitions = (definition: Graphql_ast.definition) => {
  switch (definition) {
  | Fragment({item: {fg_directives: directives}}) =>
    Result_decoder.getFragmentArgumentDefinitions(directives)
    |> List.map(((name, type_, span, type_span)) =>
         (name, type_name_to_words(type_), span, type_span)
       )
  | Operation({item: {o_variable_definitions: Some({item: definitions})}}) =>
    Graphql_ast.(
      definitions
      |> List.fold_left(
           (
             acc,
             (
               {Source_pos.item: name, span},
               {vd_type: {item: type_ref, span: type_span}},
             ),
           ) =>
             [
               (
                 name,
                 Graphql_printer.print_type(type_ref) |> type_name_to_words,
                 span,
                 type_span,
               ),
               ...acc,
             ],
           [],
         )
    )
  | _ => []
  };
};

let const_str_expr = s => Ast_helper.(Exp.constant(Pconst_string(s, None)));
let ident_from_string = (~loc=Location.none, ident) =>
  Ast_helper.(Exp.ident(~loc, {txt: Longident.parse(ident), loc}));

let make_error_raiser = message =>
  if (Ppx_config.verbose_error_handling()) {
    %expr
    Js.Exn.raiseError("graphql_ppx: " ++ [%e message]);
  } else {
    %expr
    Js.Exn.raiseError("Unexpected GraphQL query response");
  };

let string_decoder = loc => [@metaloc loc] [%expr (Obj.magic(value): string)];
let id_decoder = string_decoder;
let float_decoder = loc => [@metaloc loc] [%expr (Obj.magic(value): float)];
let int_decoder = loc => [@metaloc loc] [%expr (Obj.magic(value): int)];
let boolean_decoder = loc => [@metaloc loc] [%expr (Obj.magic(value): bool)];

let generate_poly_enum_decoder = (loc, enum_meta) => {
  let enum_match_arms =
    Ast_helper.(
      enum_meta.em_values
      |> List.map(({evm_name, _}) =>
           Exp.case(
             Pat.constant(Pconst_string(evm_name, None)),
             Exp.variant(evm_name, None),
           )
         )
    );

  let fallback_arm =
    Ast_helper.(
      Exp.case(
        Pat.var({loc: conv_loc(loc), txt: "other"}),
        Exp.variant(
          "FutureAddedValue",
          Some(
            Exp.ident({
              Location.txt: Longident.parse("other"),
              loc: conv_loc(loc),
            }),
          ),
        ),
      )
    );

  let match_expr =
    Ast_helper.(
      Exp.match(
        [%expr Obj.magic(value: string)],
        List.concat([enum_match_arms, [fallback_arm]]),
      )
    );

  let enum_ty = generate_enum_type(loc, enum_meta);

  %expr
  ([%e match_expr]: [%t enum_ty]);
};

let generate_fragment_parse_fun = (config, loc, name, arguments, definition) => {
  let ident =
    Ast_helper.Exp.ident({loc, txt: Longident.parse(name ++ ".parse")});
  let variable_defs = get_variable_definitions(definition);
  let labeled_args =
    variable_defs
    |> List.filter(((name, _, _, _)) =>
         arguments |> List.exists(arg => arg == name)
       )
    |> List.map(((arg_name, type_, _span, type_span)) =>
         (
           Labelled(arg_name),
           Ast_helper.Exp.variant(
             ~loc=config.map_loc(type_span) |> conv_loc,
             type_,
             None,
           ),
         )
       );

  Ast_helper.Exp.apply(
    ~loc,
    ident,
    List.append(labeled_args, [(Nolabel, ident_from_string("value"))]),
  );
};
let generate_solo_fragment_spread = (config, loc, name, arguments, definition) => {
  generate_fragment_parse_fun(config, loc, name, arguments, definition);
};

let generate_error = (loc, message) => {
  let ext = Ast_mapper.extension_of_error(Location.error(~loc, message));
  let%expr _value = value;
  %e
  Ast_helper.Exp.extension(~loc, ext);
};

let rec generate_parser = (config, path: list(string), definition) =>
  fun
  | Res_nullable(loc, inner) =>
    generate_nullable_decoder(config, conv_loc(loc), inner, path, definition)
  | Res_array(loc, inner) =>
    generate_array_decoder(config, conv_loc(loc), inner, path, definition)
  | Res_id(loc) => id_decoder(conv_loc(loc))
  | Res_string(loc) => string_decoder(conv_loc(loc))
  | Res_int(loc) => int_decoder(conv_loc(loc))
  | Res_float(loc) => float_decoder(conv_loc(loc))
  | Res_boolean(loc) => boolean_decoder(conv_loc(loc))
  | Res_raw_scalar(_) => [%expr value]
  | Res_poly_enum(loc, enum_meta) =>
    generate_poly_enum_decoder(loc, enum_meta)
  | Res_custom_decoder(loc, ident, inner) =>
    generate_custom_decoder(
      config,
      conv_loc(loc),
      ident,
      inner,
      path,
      definition,
    )
  | Res_record(loc, name, fields, existing_record) =>
    generate_object_decoder(
      config,
      conv_loc(loc),
      name,
      fields,
      path,
      definition,
      existing_record,
    )
  | Res_object(loc, name, fields, existing_record) =>
    generate_object_decoder(
      config,
      conv_loc(loc),
      name,
      fields,
      path,
      definition,
      existing_record,
    )
  | Res_poly_variant_union(loc, name, fragments, exhaustive) =>
    generate_poly_variant_union(
      config,
      conv_loc(loc),
      name,
      fragments,
      exhaustive,
      path,
      definition,
    )
  | Res_poly_variant_selection_set(loc, name, fields) =>
    generate_poly_variant_selection_set(
      config,
      conv_loc(loc),
      name,
      fields,
      path,
      definition,
    )

  | Res_poly_variant_interface(loc, name, base, fragments) =>
    generate_poly_variant_interface(
      config,
      conv_loc(loc),
      name,
      base,
      fragments,
      [name, ...path],
      definition,
    )
  | Res_solo_fragment_spread(loc, name, arguments) =>
    generate_solo_fragment_spread(
      config,
      conv_loc(loc),
      name,
      arguments,
      definition,
    )
  | Res_error(loc, message) => generate_error(conv_loc(loc), message)
and generate_nullable_decoder = (config, loc, inner, path, definition) =>
  [@metaloc loc]
  (
    switch%expr (Js.toOption(Obj.magic(value): Js.Nullable.t('a))) {
    | Some(_) => Some([%e generate_parser(config, path, definition, inner)])
    | None => None
    }
  )
and generate_array_decoder = (config, loc, inner, path, definition) =>
  [@metaloc loc]
  [%expr
    Obj.magic(value)
    |> Js.Array.map(value => {
         %e
         generate_parser(config, path, definition, inner)
       })
  ]
and generate_custom_decoder = (config, loc, ident, inner, path, definition) => {
  let fn_expr =
    Ast_helper.(
      Exp.ident({
        loc: Location.none,
        txt: Longident.parse(ident ++ ".parse"),
      })
    );

  [@metaloc loc]
  [%expr
    [%e fn_expr]([%e generate_parser(config, path, definition, inner)])
  ];
}
and generate_object_decoder =
    (config, loc, name, fields, path, definition, existing_record) => {
  let rec do_obj_constructor = () => {
    Ast_408.(
      Ast_helper.(
        Exp.extension((
          {txt: "bs.obj", loc},
          PStr([[%stri [%e do_obj_constructor_base()]]]),
        ))
      )
    );
  }
  and do_obj_constructor_base = () => {
    Ast_helper.(
      Exp.record(
        fields
        |> List.map(
             fun
             | Fr_named_field(key, _, inner) => (
                 {Location.txt: Longident.parse(key), loc},
                 {
                   let%expr value =
                     Js.Dict.unsafeGet(
                       Obj.magic(value),
                       [%e const_str_expr(key)],
                     );

                   %e
                   generate_parser(
                     config,
                     [key, ...path],
                     definition,
                     inner,
                   );
                 },
               )
             | Fr_fragment_spread(key, loc, name, _, arguments) => (
                 {Location.txt: Longident.parse(key), loc: conv_loc(loc)},
                 {
                   generate_fragment_parse_fun(
                     config,
                     conv_loc(loc),
                     name,
                     arguments,
                     definition,
                   );
                 },
               ),
           ),
        None,
      )
    );
  }
  and do_obj_constructor_records = () => {
    Ast_helper.(
      Exp.constraint_(
        do_obj_constructor_base(),
        Ast_helper.Typ.constr(
          {
            txt:
              switch (existing_record) {
              | None =>
                Longident.Lident(
                  Extract_type_definitions.generate_type_name(path),
                )
              | Some(type_name) => Longident.parse(type_name)
              },

            loc: Location.none,
          },
          [],
        ),
      )
    );
  }
  and obj_constructor = () => {
    [@metaloc loc]
    let%expr value = value |> Js.Json.decodeObject |> Js.Option.getExn;
    %e
    do_obj_constructor();
  }
  and obj_constructor_records = () =>
    [@metaloc loc]
    {
      do_obj_constructor_records();
    };

  config.records ? obj_constructor_records() : obj_constructor();
}
and generate_poly_variant_selection_set =
    (config, loc, name, fields, path, definition) => {
  let rec generator_loop =
    fun
    | [(field, inner), ...next] => {
        let field_name = Compat.capitalize_ascii(field);
        let variant_decoder =
          Ast_helper.(
            Exp.variant(
              field_name,
              Some(
                generate_parser(
                  config,
                  [field_name, ...path],
                  definition,
                  inner,
                ),
              ),
            )
          );

        let%expr temp =
          Js.Dict.unsafeGet(Obj.magic(value), [%e const_str_expr(field)]);

        switch (Js.Json.decodeNull(temp)) {
        | None =>
          let value = temp;
          %e
          variant_decoder;
        | Some(_) =>
          %e
          generator_loop(next)
        };
      }
    | [] =>
      make_error_raiser(
        [%expr
          "All fields on variant selection set on type "
          ++ [%e const_str_expr(name)]
          ++ " were null"
        ],
      );

  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeObject(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr
          "Expected type " ++ [%e const_str_expr(name)] ++ " to be an object"
        ],
      )
    | Some(value) =>
      %e
      generator_loop(fields)
    }
  );
}
and generate_poly_variant_interface =
    (config, loc, name, base, fragments, path, definition) => {
  let map_fallback_case = ((type_name, inner)) => {
    open Ast_helper;
    let name_pattern = Pat.any();

    Exp.variant(
      type_name,
      Some(
        generate_parser(config, [type_name, ...path], definition, inner),
      ),
    )
    |> Exp.case(name_pattern);
  };

  let map_case = ((type_name, inner)) => {
    open Ast_helper;
    let name_pattern = Pat.constant(Pconst_string(type_name, None));

    Exp.variant(
      type_name,
      Some(
        generate_parser(config, [type_name, ...path], definition, inner),
      ),
    )
    |> Exp.case(name_pattern);
  };

  let map_case_ty = ((name, _)) => {
    {
      prf_desc:
        Rtag(
          {txt: name, loc},
          false,
          [
            {
              ptyp_desc: Ptyp_any,
              ptyp_attributes: [],
              ptyp_loc: Location.none,
              ptyp_loc_stack: [],
            },
          ],
        ),
      prf_loc: Location.none,
      prf_attributes: [],
    };
  };

  let fragment_cases = List.map(map_case, fragments);
  let fallback_case = map_fallback_case(base);
  let fallback_case_ty = map_case_ty(base);

  let fragment_case_tys = List.map(map_case_ty, fragments);
  let interface_ty =
    Ast_helper.(
      Typ.variant([fallback_case_ty, ...fragment_case_tys], Closed, None)
    );
  let typename_matcher =
    Ast_helper.(
      Exp.match(
        [%expr typename],
        List.concat([fragment_cases, [fallback_case]]),
      )
    );
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeObject(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr
          "Expected Interface implementation "
          ++ [%e const_str_expr(name)]
          ++ " to be an object, got "
          ++ Js.Json.stringify(value)
        ],
      )
    | Some(typename_obj) =>
      switch (Js.Dict.get(typename_obj, "__typename")) {
      | None =>
        %e
        make_error_raiser(
          [%expr
            "Interface implementation"
            ++ [%e const_str_expr(name)]
            ++ " is missing the __typename field"
          ],
        )
      | Some(typename) =>
        switch (Js.Json.decodeString(typename)) {
        | None =>
          %e
          make_error_raiser(
            [%expr
              "Interface implementation "
              ++ [%e const_str_expr(name)]
              ++ " has a __typename field that is not a string"
            ],
          )
        | Some(typename) => ([%e typename_matcher]: [%t interface_ty])
        }
      }
    }
  );
}
and generate_poly_variant_union =
    (config, loc, name, fragments, exhaustive_flag, path, definition) => {
  let fragment_cases =
    Ast_helper.(
      fragments
      |> List.map(((type_name, inner)) => {
           let name_pattern = Pat.constant(Pconst_string(type_name, None));
           Ast_helper.(
             Exp.variant(
               type_name,
               Some(
                 generate_parser(
                   config,
                   [type_name, ...path],
                   definition,
                   inner,
                 ),
               ),
             )
           )
           |> Exp.case(name_pattern);
         })
    );
  let (fallback_case, fallback_case_ty) =
    Ast_helper.(
      switch (exhaustive_flag) {
      | Result_structure.Exhaustive => (
          Exp.case(
            Pat.var({loc: Location.none, txt: "typename"}),
            make_error_raiser(
              [%expr
                "Union "
                ++ [%e const_str_expr(name)]
                ++ " returned unknown type "
                ++ typename
              ],
            ),
          ),
          [],
        )
      | Nonexhaustive => (
          Exp.case(Pat.any(), [%expr `Nonexhaustive]),
          [
            {
              prf_desc: Rtag({txt: "Nonexhaustive", loc}, true, []),
              prf_loc: loc,
              prf_attributes: [],
            },
          ],
        )
      }
    );
  let fragment_case_tys =
    fragments
    |> List.map(((name, _)) =>
         {
           prf_desc:
             Rtag(
               {txt: name, loc},
               false,
               [
                 {
                   ptyp_desc: Ptyp_any,
                   ptyp_attributes: [],
                   ptyp_loc_stack: [],
                   ptyp_loc: Location.none,
                 },
               ],
             ),
           prf_attributes: [],
           prf_loc: Location.none,
         }
       );

  let union_ty =
    Ast_helper.(
      Typ.variant(
        List.concat([fallback_case_ty, fragment_case_tys]),
        Closed,
        None,
      )
    );
  let typename_matcher =
    Ast_helper.(
      Exp.match(
        [%expr typename],
        List.concat([fragment_cases, [fallback_case]]),
      )
    );
  [@metaloc loc]
  (
    switch%expr (Js.Json.decodeObject(value)) {
    | None =>
      %e
      make_error_raiser(
        [%expr
          "Expected union "
          ++ [%e const_str_expr(name)]
          ++ " to be an object, got "
          ++ Js.Json.stringify(value)
        ],
      )
    | Some(typename_obj) =>
      switch (Js.Dict.get(typename_obj, "__typename")) {
      | None =>
        %e
        make_error_raiser(
          [%expr
            "Union "
            ++ [%e const_str_expr(name)]
            ++ " is missing the __typename field"
          ],
        )
      | Some(typename) =>
        switch (Js.Json.decodeString(typename)) {
        | None =>
          %e
          make_error_raiser(
            [%expr
              "Union "
              ++ [%e const_str_expr(name)]
              ++ " has a __typename field that is not a string"
            ],
          )
        | Some(typename) => ([%e typename_matcher]: [%t union_ty])
        }
      }
    }
  );
};
