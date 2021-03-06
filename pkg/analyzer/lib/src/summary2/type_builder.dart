// Copyright (c) 2019, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:analyzer/src/summary/format.dart';
import 'package:analyzer/src/summary/idl.dart';
import 'package:analyzer/src/summary2/linked_bundle_context.dart';
import 'package:analyzer/src/summary2/reference_resolver.dart';

/// Build types in a [TypesToBuild].
class TypeBuilder {
  final LinkedBundleContext bundleContext;

  TypeBuilder(this.bundleContext);

  void build(TypesToBuild typesToBuild) {
    for (var node in typesToBuild.typeNames) {
      _buildTypeName(node);
    }
    for (var node in typesToBuild.declarations) {
      _setTypesForDeclaration(node);
    }
  }

  void _buildTypeName(LinkedNodeBuilder node) {
    var referenceIndex = _typeNameElementIndex(node.typeName_name);
    var reference = bundleContext.referenceOfIndex(referenceIndex);

    var typeArguments = const <LinkedNodeTypeBuilder>[];
    var typeArgumentList = node.typeName_typeArguments;
    if (typeArgumentList != null) {
      typeArguments = typeArgumentList.typeArgumentList_arguments
          .map((node) => _getType(node))
          .toList();
    }

    if (reference.isClass) {
      node.typeName_type = LinkedNodeTypeBuilder(
        kind: LinkedNodeTypeKind.interface,
        interfaceClass: referenceIndex,
        interfaceTypeArguments: typeArguments,
      );
    } else if (reference.isEnum) {
      node.typeName_type = LinkedNodeTypeBuilder(
        kind: LinkedNodeTypeKind.interface,
        interfaceClass: referenceIndex,
      );
    } else if (reference.isTypeParameter) {
      node.typeName_type = LinkedNodeTypeBuilder(
        kind: LinkedNodeTypeKind.typeParameter,
        typeParameterParameter: referenceIndex,
      );
    } else {
      // TODO(scheglov) set Object? keep unresolved?
      throw UnimplementedError();
    }
  }

  void _setTypesForDeclaration(LinkedNodeBuilder node) {
    var kind = node.kind;
    if (kind == LinkedNodeKind.fieldFormalParameter) {
      node.fieldFormalParameter_type2 = _getType(
        node.fieldFormalParameter_type,
      );
    } else if (kind == LinkedNodeKind.functionDeclaration) {
      node.functionDeclaration_returnType2 = _getType(
        node.functionDeclaration_returnType,
      );
    } else if (kind == LinkedNodeKind.functionTypeAlias) {
      node.functionTypeAlias_returnType2 = _getType(
        node.functionTypeAlias_returnType,
      );
    } else if (kind == LinkedNodeKind.methodDeclaration) {
      node.methodDeclaration_returnType2 = _getType(
        node.methodDeclaration_returnType,
      );
    } else if (kind == LinkedNodeKind.simpleFormalParameter) {
      node.simpleFormalParameter_type2 = _getType(
        node.simpleFormalParameter_type,
      );
    } else if (kind == LinkedNodeKind.variableDeclarationList) {
      var typeNode = node.variableDeclarationList_type;
      for (var variable in node.variableDeclarationList_variables) {
        variable.variableDeclaration_type2 = _getType(typeNode);
      }
    } else {
      throw UnimplementedError('$kind');
    }
  }

  static LinkedNodeTypeBuilder _getType(LinkedNodeBuilder node) {
    var kind = node.kind;
    if (kind == LinkedNodeKind.typeName) {
      return node.typeName_type;
    } else {
      throw UnimplementedError('$kind');
    }
  }

  static int _typeNameElementIndex(LinkedNode name) {
    if (name.kind == LinkedNodeKind.simpleIdentifier) {
      return name.simpleIdentifier_element;
    } else {
      // TODO(scheglov) implement
      throw UnimplementedError();
    }
  }
}
